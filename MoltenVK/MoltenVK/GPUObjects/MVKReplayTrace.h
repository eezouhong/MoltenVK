// Optional local replay diagnostics. Never emits per-draw log lines.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <time.h>

namespace mvkreplay {
enum Region : uint32_t {
    SPIRVToMSL, MSLLibrary, MSLFunction, IRLibrary, IRFunction, IRReflection,
    MetalGraphicsPSO, MetalComputePSO, DescriptorUpdate, IRShadowUpdate,
    DescriptorBinding, IRRootBinding, ParameterCopy, IndirectParameters,
    MetalCommandEncoding, RegionCount
};
struct Sample {uint64_t calls,wallNs,threadCpuNs,cpuUnavailable,maxWallNs;};
struct Counter {
    std::atomic<uint64_t> calls{0},wallNs{0},threadCpuNs{0},cpuUnavailable{0},maxWallNs{0};
};
inline Counter counters[RegionCount];
enum class Mode { Off, Detailed, Coarse };
inline Mode mode() {
    static Mode value=[] {
        const char* p=getenv("MELONX_PIPELINE_REPLAY_TRACE");
        if (p && !strcmp(p,"1")) return Mode::Detailed;
        if (p && !strcmp(p,"coarse")) return Mode::Coarse;
        return Mode::Off;
    }();
    return value;
}
inline bool enabled() { return mode()!=Mode::Off; }
inline uint64_t nanoseconds(clockid_t clock,bool& valid) {
    timespec time{};valid=clock_gettime(clock,&time)==0;
    return valid?uint64_t(time.tv_sec)*1000000000+time.tv_nsec:0;
}
class Timer {
    Region region;uint64_t wall=0,cpu=0;bool cpuValid=false;
public:
    explicit Timer(Region r):region(r) {
        const auto traceMode=mode();
        // Game speed comparisons time complete command encoding batches, not
        // every tiny draw/descriptor operation. Detailed replay remains opt-in.
        if (traceMode==Mode::Off || (traceMode==Mode::Coarse && r>=DescriptorUpdate && r!=MetalCommandEncoding)) return;
        bool valid;wall=nanoseconds(CLOCK_MONOTONIC,valid);
        cpu=nanoseconds(CLOCK_THREAD_CPUTIME_ID,cpuValid);
    }
    ~Timer() {
        if (!wall) return;
        bool wallValid,endCpuValid;
        uint64_t end=nanoseconds(CLOCK_MONOTONIC,wallValid),endCpu=nanoseconds(CLOCK_THREAD_CPUTIME_ID,endCpuValid);
        if (!wallValid || end<wall) return;
        auto& c=counters[region];uint64_t elapsed=end-wall;
        c.calls.fetch_add(1,std::memory_order_relaxed);c.wallNs.fetch_add(elapsed,std::memory_order_relaxed);
        if (cpuValid && endCpuValid && endCpu>=cpu) c.threadCpuNs.fetch_add(endCpu-cpu,std::memory_order_relaxed);
        else c.cpuUnavailable.fetch_add(1,std::memory_order_relaxed);
        uint64_t maximum=c.maxWallNs.load(std::memory_order_relaxed);
        while (maximum<elapsed && !c.maxWallNs.compare_exchange_weak(maximum,elapsed,std::memory_order_relaxed)) {}
    }
};
inline uint32_t snapshot(Sample* output,uint32_t capacity,bool reset) {
    if (!enabled() || !output || capacity<RegionCount) return 0;
    for (uint32_t i=0;i<RegionCount;++i) {
        auto& c=counters[i];auto get=[&](std::atomic<uint64_t>& x){return reset?x.exchange(0,std::memory_order_relaxed):x.load(std::memory_order_relaxed);};
        output[i]={get(c.calls),get(c.wallNs),get(c.threadCpuNs),get(c.cpuUnavailable),get(c.maxWallNs)};
    }
    return RegionCount;
}
struct GPUCounter {
    std::atomic<uint64_t> calls{0},wallNs{0},unavailable{0},errors{0},lastDumpNs{0};
    std::atomic<uint64_t> irIndirectBatches{0},irIndirectDraws{0},irIndirectDispatches{0},irPassBreaks{0},irTemporaryBytes{0},irDirectUploads{0};
    std::atomic<uint64_t> renderEncoders{0},computeEncoders{0},blitEncoders{0};
};
inline GPUCounter gpu;
struct SubmissionSample {
    uint64_t buffers,gpuNs,unavailable,errors,indirectBatches,indirectDraws,
        indirectDispatches,passBreaks,temporaryBytes,directUploads,renderEncoders,
        computeEncoders,blitEncoders;
};
inline bool submissionSnapshot(SubmissionSample* output) {
    if (mode()!=Mode::Coarse || !output) return false;
    auto get=[](std::atomic<uint64_t>& value){return value.load(std::memory_order_relaxed);};
    *output={get(gpu.calls),get(gpu.wallNs),get(gpu.unavailable),get(gpu.errors),
        get(gpu.irIndirectBatches),get(gpu.irIndirectDraws),get(gpu.irIndirectDispatches),
        get(gpu.irPassBreaks),get(gpu.irTemporaryBytes),get(gpu.irDirectUploads),
        get(gpu.renderEncoders),get(gpu.computeEncoders),get(gpu.blitEncoders)};
    return true;
}
inline void encoderStarted(unsigned kind) {
    if (mode()!=Mode::Coarse) return;
    auto& counter=kind==0?gpu.renderEncoders:kind==1?gpu.computeEncoders:gpu.blitEncoders;
    counter.fetch_add(1,std::memory_order_relaxed);
}
inline void indirectRuntime(uint32_t draws,uint64_t bytes,bool passBreak,bool compute) {
    if (mode()!=Mode::Coarse) return;
    if (compute) gpu.irIndirectDispatches.fetch_add(1,std::memory_order_relaxed);
    else {
        gpu.irIndirectBatches.fetch_add(1,std::memory_order_relaxed);
        gpu.irIndirectDraws.fetch_add(draws,std::memory_order_relaxed);
    }
    if (passBreak) gpu.irPassBreaks.fetch_add(1,std::memory_order_relaxed);
    gpu.irTemporaryBytes.fetch_add(bytes,std::memory_order_relaxed);
}
inline void directRuntimeUpload() {
    if (mode()==Mode::Coarse) gpu.irDirectUploads.fetch_add(1,std::memory_order_relaxed);
}
inline void commandBufferCompleted(double start,double end,bool success) {
    if (mode()!=Mode::Coarse) return;
    gpu.calls.fetch_add(1,std::memory_order_relaxed);
    if (!success) gpu.errors.fetch_add(1,std::memory_order_relaxed);
    if (success && std::isfinite(start) && std::isfinite(end) && start>0 && end>=start) {
        gpu.wallNs.fetch_add(uint64_t((end-start)*1e9),std::memory_order_relaxed);
    } else gpu.unavailable.fetch_add(1,std::memory_order_relaxed);

    bool valid;const uint64_t now=nanoseconds(CLOCK_MONOTONIC,valid);
    uint64_t before=gpu.lastDumpNs.load(std::memory_order_relaxed);
    if (!valid || now<before || now-before<1000000000 ||
        !gpu.lastDumpNs.compare_exchange_strong(before,now,std::memory_order_relaxed)) return;
    Sample values[RegionCount];snapshot(values,RegionCount,false);
    static const char* names[]={"spirv_to_msl","msl_library","msl_function","ir_library","ir_function","ir_reflection","graphics_pso","compute_pso","descriptor_update","ir_shadow_update","descriptor_binding","ir_root_binding","parameter_copy","indirect_parameters","native_encoding"};
    char line[4096];size_t used=snprintf(line,sizeof(line),"MELONX_REPLAY_BATCH_TOTALS {\"monotonicNs\":%llu,\"inclusive\":true,\"regions\":[",(unsigned long long)now);
    bool comma=false;
    for (uint32_t i=0;i<RegionCount;++i) {
        if (!values[i].calls) continue;
        const auto& v=values[i];
        used+=snprintf(line+used,sizeof(line)-used,"%s{\"region\":\"%s\",\"calls\":%llu,\"wallNs\":%llu,\"threadCpuNs\":%llu,\"cpuUnavailable\":%llu}",comma?",":"",names[i],(unsigned long long)v.calls,(unsigned long long)v.wallNs,(unsigned long long)v.threadCpuNs,(unsigned long long)v.cpuUnavailable);
        comma=true;
    }
    snprintf(line+used,sizeof(line)-used,"],\"gpuCommandBuffers\":%llu,\"gpuCommandBufferNs\":%llu,\"gpuUnavailable\":%llu,\"gpuErrors\":%llu,\"irIndirectBatches\":%llu,\"irIndirectDraws\":%llu,\"irIndirectDispatches\":%llu,\"irPassBreaks\":%llu,\"irTemporaryBytes\":%llu,\"irDirectUploads\":%llu}",(unsigned long long)gpu.calls.load(std::memory_order_relaxed),(unsigned long long)gpu.wallNs.load(std::memory_order_relaxed),(unsigned long long)gpu.unavailable.load(std::memory_order_relaxed),(unsigned long long)gpu.errors.load(std::memory_order_relaxed),(unsigned long long)gpu.irIndirectBatches.load(std::memory_order_relaxed),(unsigned long long)gpu.irIndirectDraws.load(std::memory_order_relaxed),(unsigned long long)gpu.irIndirectDispatches.load(std::memory_order_relaxed),(unsigned long long)gpu.irPassBreaks.load(std::memory_order_relaxed),(unsigned long long)gpu.irTemporaryBytes.load(std::memory_order_relaxed),(unsigned long long)gpu.irDirectUploads.load(std::memory_order_relaxed));
    // Cumulative counters, one line per second; concurrent batches may overlap.
    // GPUStartTime..GPUEndTime is buffer elapsed time, including GPU waits, not
    // shader-only execution. Calling-thread CPU excludes compiler helpers.
    fprintf(stderr,"%s\n",line);
}
}
