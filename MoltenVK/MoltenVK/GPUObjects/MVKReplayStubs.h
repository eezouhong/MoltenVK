#pragma once
#include <cstdint>
#include <time.h>
namespace mvkreplay {
enum Region : uint32_t {
    SPIRVToMSL, MSLLibrary, MSLFunction, IRLibrary, IRFunction, IRReflection,
    MetalGraphicsPSO, MetalComputePSO, DescriptorUpdate, IRShadowUpdate,
    DescriptorBinding, IRRootBinding, ParameterCopy, IndirectParameters,
    MetalCommandEncoding, RegionCount
};
enum class Mode { Off, Detailed, Coarse };
struct Sample { uint64_t calls,wallNs,threadCpuNs,cpuUnavailable,maxWallNs; };
enum class DescriptorRangeKind : unsigned { Write, Shadow };
struct DescriptorRangeSample { uint64_t calls=0,samples=0,wallNs=0,unavailable=0; };
struct DescriptorSampleData {
    uint64_t batches=0,sampledBatches=0,wallNs=0,cpuNs=0,unavailable=0;
    DescriptorRangeSample ranges[2];
};
class DescriptorSampler {
public:
    static constexpr uint32_t inverseProbability=128;
    explicit constexpr DescriptorSampler(uint32_t) noexcept {}
    constexpr bool next() noexcept { return false; }
};
enum class BindingGroup : unsigned { Resources, DrawPreparation, MetalDraw, Residency, Count };
constexpr unsigned bindingCounterCount=unsigned(BindingGroup::Count)*2;
struct BindingSample {
    uint64_t calls=0,samples=0,wallNs=0,cpuNs=0,unavailable=0;
    uint64_t parts[3]={};
};
inline constexpr Mode mode() noexcept { return Mode::Off; }
inline constexpr Mode setMode(Mode) noexcept { return Mode::Off; }
inline constexpr bool enabled() noexcept { return false; }
inline constexpr bool descriptorTimingEnabled() noexcept { return false; }
inline constexpr bool descriptorSamplingEnabled() noexcept { return false; }
inline constexpr bool bindingSamplingEnabled() noexcept { return false; }
inline constexpr uint64_t descriptorClock(clockid_t) noexcept { return 0; }
inline uint64_t nanoseconds(clockid_t,bool& valid) noexcept { valid=false;return 0; }
class Timer {
public:
    explicit constexpr Timer(Region,Mode=Mode::Off) noexcept {}
};
class DescriptorRangeTimer {
public:
    explicit constexpr DescriptorRangeTimer(Region) noexcept {}
};
class DescriptorBatchTrace {
public:
    explicit constexpr DescriptorBatchTrace(bool) noexcept {}
};
class DescriptorSampleScope {
public:
    constexpr DescriptorSampleScope(DescriptorRangeKind,bool) noexcept {}
};
class BindingTrace {
public:
    explicit constexpr BindingTrace(bool,bool=false,BindingGroup=BindingGroup::Resources) noexcept {}
    constexpr void checkpoint() noexcept {}
};
struct SubmissionSample {
    uint64_t buffers,gpuNs,unavailable,errors,indirectBatches,indirectDraws,
        indirectDispatches,passBreaks,temporaryBytes,directUploads,renderEncoders,
        computeEncoders,blitEncoders;
};
inline constexpr uint32_t snapshot(Sample*,uint32_t,bool) noexcept { return 0; }
inline constexpr uint32_t bindingSnapshot(BindingSample*,uint32_t) noexcept { return 0; }
inline constexpr uint32_t bindingCalibrationSnapshot(BindingSample*,uint32_t) noexcept { return 0; }
inline constexpr bool submissionSnapshot(SubmissionSample*) noexcept { return false; }
inline constexpr void encoderStarted(unsigned) noexcept {}
inline constexpr void indirectRuntime(uint32_t,uint64_t,bool,bool) noexcept {}
inline constexpr void directRuntimeUpload() noexcept {}
inline constexpr void indirectInvocation(uint32_t,bool) noexcept {}
inline constexpr void renderPassInterrupted() noexcept {}
inline constexpr uint64_t frameBufferCreated() noexcept { return 0; }
inline constexpr void frameBufferCompleted(uint64_t,double,double,bool) noexcept {}
inline constexpr uint64_t framePresented() noexcept { return 0; }
inline constexpr void commandBufferCompleted(double,double,bool) noexcept {}
}
