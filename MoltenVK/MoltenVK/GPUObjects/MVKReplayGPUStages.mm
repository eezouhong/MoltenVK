#include "MVKReplayConfig.h"
#if MVK_REPLAY_TRACE
#include "MVKReplayGPUStages.h"
#include "MVKReplayGPUStageData.h"
#include "MVKReplayTrace.h"
#include <array>
#include <unordered_map>
#include <sstream>
#include <iomanip>

namespace mvkreplay {
static bool stagesEnabled() {
    static const bool requested=[] { const char* p=getenv("MELONX_REPLAY_GPU_STAGES"); return p&&!strcmp(p,"1"); }();
    return (requested || chainSamplingEnabled()) && mode()==Mode::Coarse;
}
static GPUStageClock clockPair(id<MTLDevice> device) {
    bool validA,validB;uint64_t a=nanoseconds(CLOCK_MONOTONIC,validA);
    MTLTimestamp cpu=0,gpu=0;[device sampleTimestamps:&cpu gpuTimestamp:&gpu];
    uint64_t b=nanoseconds(CLOCK_MONOTONIC,validB);
    if (!validA||!validB||b<a) return {};
    return {a+(b-a)/2,b-a,cpu,gpu};
}
class GPUStagePool {
public:
    std::mutex lock;
    id<MTLDevice> device;
    std::array<id<MTLCounterSampleBuffer>,StageSlotAllocator::capacity> buffers{};
    GPUStageEpochTracker epochs;
    uint32_t unavailable=0;
    uint64_t seed=1,poolId=0;
    unsigned inverseProbability=16;
    GPUStageClock anchor,previousAnchor;
    explicit GPUStagePool(id<MTLDevice> d):device([d retain]) {
        bool valid;seed=nanoseconds(CLOCK_MONOTONIC,valid);
        const char* rate=getenv("MELONX_REPLAY_GPU_STAGE_EPOCH_RATE");if(rate&&!strcmp(rate,"1"))inverseProbability=1;
        if (@available(macOS 11.0,iOS 14.0,tvOS 14.0,*)) {
            if (![device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]) { unavailable=StageUnsupported;return; }
            id<MTLCounterSet> timestamps=nil;
            for (id<MTLCounterSet> set in device.counterSets)
                if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) timestamps=set;
            if (!timestamps) { unavailable=StageUnsupported;return; }
            auto* descriptor=[MTLCounterSampleBufferDescriptor new];
            descriptor.counterSet=timestamps;descriptor.sampleCount=4096;descriptor.storageMode=MTLStorageModeShared;
            for (auto& buffer:buffers) {
                NSError* error=nil;buffer=[device newCounterSampleBufferWithDescriptor:descriptor error:&error];
                if (!buffer) { unavailable=StageUnsupported;break; }
            }
            [descriptor release];
            anchor=clockPair(device);
        } else unavailable=StageUnsupported;
    }
    ~GPUStagePool() { for(auto buffer:buffers)[buffer release];[device release]; }
    bool selected(uint64_t frame) const {return inverseProbability==1||sampleGPUStageEpoch(frame,seed);}
    std::shared_ptr<GPUStageEpoch> begin(uint64_t frame) {
        std::lock_guard<std::mutex> guard(lock);return epochs.begin(frame);
    }
    void complete(const std::shared_ptr<GPUStageEpoch>& epoch) {
        std::lock_guard<std::mutex> guard(lock);epochs.complete(epoch);
    }
    void seal(uint64_t frame) {
        std::lock_guard<std::mutex> guard(lock);epochs.seal(frame);
    }
    struct Calibration { GPUStageClock before;std::optional<double> scale; };
    Calibration calibrate(const GPUStageClock& after) {
        std::lock_guard<std::mutex> guard(lock);
        auto before=anchor;
        if(after.hostMidNs>=anchor.hostMidNs&&after.hostMidNs-anchor.hostMidNs<250000000&&previousAnchor.hostMidNs)
            before=previousAnchor;
        auto scale=stageClockScale(before,after);
        if(scale&&after.hostMidNs>anchor.hostMidNs&&after.hostMidNs-anchor.hostMidNs>=1000000000) {
            previousAnchor=anchor;anchor=after;
        }
        return {before,scale};
    }
};
class GPUStageCapture {
public:
    std::shared_ptr<GPUStagePool> pool;
    std::mutex lock;
    uint64_t id=0,frame=0;
    void* key=nullptr;
    std::array<uint32_t,3> encoders{},unmeasured{};
    std::array<uint8_t,256> kinds{};
    std::array<bool,256> draws{};
    std::array<GPUComputeWork,256> computeWork{};
    std::array<GPUDrawWork,256> drawWork{};
    std::array<std::array<uint64_t,3>,256> renderExtent{};
    std::array<uint64_t,256> fenceWaits{},fenceHash{};
    uint32_t sampledEncoders=0,unavailable=0;
    std::array<unsigned,256> indices{};
    std::shared_ptr<GPUStageEpoch> epoch;
    bool completed=false;
    GPUStageClock before;
    GPUStageCapture(std::shared_ptr<GPUStagePool> p):pool(std::move(p)) {}
    ~GPUStageCapture() { if(!completed)pool->complete(epoch); }
};
struct StageRegistry {
    std::mutex lock;
    std::unordered_map<void*,std::weak_ptr<GPUStageCapture>> captures;
    std::unordered_map<void*,std::weak_ptr<GPUStagePool>> pools;
    uint64_t nextId=1,nextPoolId=1;
};
static StageRegistry& registry() { static StageRegistry value;return value; }
std::shared_ptr<GPUStagePool> createGPUStagePool(id<MTLDevice> device) {
    if (!stagesEnabled()||!device) return {};
    auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);
    void* key=(void*)device;auto existing=r.pools[key].lock();if(existing)return existing;
    for(auto i=r.pools.begin();i!=r.pools.end();) { if(i->second.expired())i=r.pools.erase(i);else++i; }
    auto pool=std::make_shared<GPUStagePool>(device);pool->poolId=r.nextPoolId++;r.pools[key]=pool;return pool;
}
std::shared_ptr<GPUStageCapture> beginGPUStages(const std::shared_ptr<GPUStagePool>& pool,id<MTLCommandBuffer> buffer,uint64_t frame) {
    if(!pool||!buffer||!pool->selected(frame))return {};
    auto capture=std::make_shared<GPUStageCapture>(pool);capture->frame=frame;capture->key=(void*)buffer;capture->epoch=pool->begin(frame);
    if(!capture->epoch)capture->unavailable|=StagePoolFull;
    if (@available(macOS 11.0,iOS 14.0,tvOS 14.0,*)) capture->before=clockPair(pool->device);
    else capture->unavailable|=StageUnsupported;
    auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);capture->id=r.nextId++;
    if(r.captures.size()>=256) {
        for(auto i=r.captures.begin();i!=r.captures.end();) { if(i->second.expired())i=r.captures.erase(i);else++i; }
    }
    if(r.captures.size()>=256)capture->unavailable|=StageRegistryFull;
    else r.captures[capture->key]=capture;
    return capture;
}
struct StageAttachment { id<MTLCounterSampleBuffer> buffer=nil;unsigned index=0; };
static StageAttachment reserveAttachment(id<MTLCommandBuffer> buffer,uint8_t kind,bool occupied=false) {
    if(!stagesEnabled()||!buffer)return {};
    std::shared_ptr<GPUStageCapture> capture;
    {auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);auto i=r.captures.find((void*)buffer);if(i!=r.captures.end())capture=i->second.lock();}
    if(!capture)return {};
    std::lock_guard<std::mutex> guard(capture->lock);++capture->encoders[kind];
    if(capture->pool->unavailable){capture->unavailable|=capture->pool->unavailable;return {};}
    if(capture->sampledEncoders>=256){capture->unavailable|=StagePassLimit;return {};}
    if(occupied){capture->unavailable|=StageAttachmentUsed;return {};}
    if(!capture->epoch){capture->unavailable|=StagePoolFull;return {};}
    auto& pool=*capture->pool;std::lock_guard<std::mutex> poolGuard(pool.lock);auto& epoch=*capture->epoch;
    if(epoch.nextEncoder>=1024){capture->unavailable|=StagePassLimit;return {};}
    auto allocation=pool.epochs.reserve(capture->epoch);
    if(!allocation){capture->unavailable|=StagePoolFull;return {};}
    unsigned index=capture->sampledEncoders++;capture->kinds[index]=kind;
    capture->indices[index]=allocation->index;
    return {pool.buffers[allocation->slot],allocation->index};
}
bool attachGPUStages(id<MTLCommandBuffer> buffer,MTLRenderPassDescriptor* pass) {
    if(!stagesEnabled()||!pass)return false;
    if (@available(macOS 11.0,iOS 14.0,tvOS 14.0,*)) {
        auto attachment=pass.sampleBufferAttachments[0];
        auto sample=reserveAttachment(buffer,0,attachment.sampleBuffer!=nil);
        if(!sample.buffer)return false;
        attachment.sampleBuffer=sample.buffer;
        attachment.startOfVertexSampleIndex=sample.index;attachment.endOfVertexSampleIndex=sample.index+1;
        attachment.startOfFragmentSampleIndex=sample.index+2;attachment.endOfFragmentSampleIndex=sample.index+3;
        std::shared_ptr<GPUStageCapture> capture;
        {auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);auto i=r.captures.find((void*)buffer);if(i!=r.captures.end())capture=i->second.lock();}
        if(capture) {
            std::lock_guard<std::mutex> guard(capture->lock);
            capture->renderExtent[capture->sampledEncoders-1]={pass.renderTargetWidth,pass.renderTargetHeight,pass.renderTargetArrayLength};
        }
        return true;
    }
    return false;
}
MTLComputePassDescriptor* computeGPUStagePass(id<MTLCommandBuffer> buffer,MTLDispatchType dispatch) {
    if(!stagesEnabled())return nil;
    if (@available(macOS 11.0,iOS 14.0,tvOS 14.0,*)) {
        auto sample=reserveAttachment(buffer,1);if(!sample.buffer)return nil;
        auto* pass=[MTLComputePassDescriptor computePassDescriptor];pass.dispatchType=dispatch;
        auto attachment=pass.sampleBufferAttachments[0];attachment.sampleBuffer=sample.buffer;
        attachment.startOfEncoderSampleIndex=sample.index;attachment.endOfEncoderSampleIndex=sample.index+1;
        return pass;
    }
    return nil;
}
MTLBlitPassDescriptor* blitGPUStagePass(id<MTLCommandBuffer> buffer) {
    if(!stagesEnabled())return nil;
    if (@available(macOS 11.0,iOS 14.0,tvOS 14.0,*)) {
        auto sample=reserveAttachment(buffer,2);if(!sample.buffer)return nil;
        auto* pass=[MTLBlitPassDescriptor blitPassDescriptor];
        auto attachment=pass.sampleBufferAttachments[0];attachment.sampleBuffer=sample.buffer;
        attachment.startOfEncoderSampleIndex=sample.index;attachment.endOfEncoderSampleIndex=sample.index+1;
        return pass;
    }
    return nil;
}
void sealGPUStageEpoch(uint64_t frame) {
    if(!stagesEnabled()||!frame)return;
    std::vector<std::shared_ptr<GPUStagePool>> pools;
    {auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);for(auto& entry:r.pools)if(auto p=entry.second.lock())pools.push_back(p);}
    for(auto& pool:pools)pool->seal(frame);
}
void noteUnmeasuredGPUStage(id<MTLCommandBuffer> buffer,unsigned kind) {
    if(!stagesEnabled()||!buffer||kind>=3)return;
    std::shared_ptr<GPUStageCapture> capture;
    {auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);auto i=r.captures.find((void*)buffer);if(i!=r.captures.end())capture=i->second.lock();}
    if(!capture)return;
    std::lock_guard<std::mutex> guard(capture->lock);++capture->encoders[kind];++capture->unmeasured[kind];
}
bool gpuStageTracingEnabled() { return stagesEnabled(); }
void noteGPUStageDraw(id<MTLCommandBuffer> buffer) {
    if(!stagesEnabled()||!buffer)return;
    std::shared_ptr<GPUStageCapture> capture;
    {auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);auto i=r.captures.find((void*)buffer);if(i!=r.captures.end())capture=i->second.lock();}
    if(!capture)return;
    std::lock_guard<std::mutex> guard(capture->lock);
    if(capture->sampledEncoders&&capture->kinds[capture->sampledEncoders-1]==0) {
        auto i=capture->sampledEncoders-1;capture->draws[i]=true;
        capture->drawWork[i].add(0,0,0,0,false,false);
    }
}
void noteGPUStageRenderWork(id<MTLCommandBuffer> buffer,const GPUDrawWork& work) {
    if(!stagesEnabled()||!buffer||!work.draws)return;
    std::shared_ptr<GPUStageCapture> capture;
    {auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);auto i=r.captures.find((void*)buffer);if(i!=r.captures.end())capture=i->second.lock();}
    if(!capture)return;
    std::lock_guard<std::mutex> guard(capture->lock);
    if(capture->sampledEncoders&&capture->kinds[capture->sampledEncoders-1]==0) {
        auto i=capture->sampledEncoders-1;capture->draws[i]=true;
        // Internal clears/blits are recorded directly. Preserve their unknown
        // work marker instead of replacing it with the Vulkan draw aggregate.
        auto internal=capture->drawWork[i];capture->drawWork[i]=work;
        if(internal.draws) {
            capture->drawWork[i].draws+=internal.draws;
            capture->drawWork[i].unknownDraws+=internal.draws;
            capture->drawWork[i].mixedPrograms=true;
        }
    }
}
void noteGPUStageDispatch(id<MTLCommandBuffer> buffer,uint64_t program,uint64_t x,uint64_t y,uint64_t z,
                          uint64_t tx,uint64_t ty,uint64_t tz,bool indirect) {
    if(!stagesEnabled()||!buffer)return;
    std::shared_ptr<GPUStageCapture> capture;
    {auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);auto i=r.captures.find((void*)buffer);if(i!=r.captures.end())capture=i->second.lock();}
    if(!capture)return;
    std::lock_guard<std::mutex> guard(capture->lock);
    if(capture->sampledEncoders&&capture->kinds[capture->sampledEncoders-1]==1)
        capture->computeWork[capture->sampledEncoders-1].add(program,x,y,z,tx,ty,tz,indirect);
}
void noteGPUStageFenceWait(id<MTLCommandBuffer> buffer,unsigned source,unsigned target,uint64_t index) {
    if(!stagesEnabled()||!buffer)return;
    std::shared_ptr<GPUStageCapture> capture;
    {auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);auto i=r.captures.find((void*)buffer);if(i!=r.captures.end())capture=i->second.lock();}
    if(!capture)return;
    std::lock_guard<std::mutex> guard(capture->lock);if(!capture->sampledEncoders)return;
    auto i=capture->sampledEncoders-1;++capture->fenceWaits[i];
    auto& hash=capture->fenceHash[i];hash^=uint64_t(source)|(uint64_t(target)<<16)|(index<<32);hash*=1099511628211ULL;
}
void finishGPUStages(const std::shared_ptr<GPUStageCapture>& capture,id<MTLCommandBuffer> buffer) {
    if(!capture)return;
    {auto& r=registry();std::lock_guard<std::mutex> guard(r.lock);auto i=r.captures.find(capture->key);
     if(i!=r.captures.end()&&i->second.lock()==capture)r.captures.erase(i);}
    @autoreleasepool {
        std::lock_guard<std::mutex> guard(capture->lock);
        GPUStageClock after;GPUStageDurations durations;std::array<uint64_t,1024> ticks{};
        if (@available(macOS 11.0,iOS 14.0,tvOS 14.0,*)) {
            after=clockPair(capture->pool->device);
            if(capture->sampledEncoders&&capture->epoch&&capture->epoch->slot) {
                // Resolve only ranges written by this completed command buffer.
                // Other command buffers share the epoch buffer at disjoint indices.
                for(unsigned first=0;first<capture->sampledEncoders;) {
                    unsigned end=first+1;
                    while(end<capture->sampledEncoders&&capture->indices[end]==capture->indices[end-1]+4)++end;
                    NSData* data=[capture->pool->buffers[*capture->epoch->slot] resolveCounterRange:NSMakeRange(capture->indices[first],4*(end-first))];
                    if(data.length!=4*(end-first)*sizeof(MTLCounterResultTimestamp))capture->unavailable|=StageResolveFailed;
                    else {
                        auto* values=static_cast<const MTLCounterResultTimestamp*>(data.bytes);
                        for(unsigned i=0;i<4*(end-first);++i)ticks[4*first+i]=values[i].timestamp;
                    }
                    first=end;
                }
                durations=stageDurations(ticks.data(),capture->kinds.data(),capture->sampledEncoders,capture->before.gpuTicks,after.gpuTicks,capture->draws.data());
                if(durations.invalidPasses)capture->unavailable|=StageInvalidTimestamp;
            }
        }
        auto calibration=capture->pool->calibrate(after);auto scale=calibration.scale;
        if(!scale)capture->unavailable|=StageInvalidClock;
        const bool success=buffer.status==MTLCommandBufferStatusCompleted;
        bool clockValid;uint64_t completed=nanoseconds(CLOCK_MONOTONIC,clockValid);
        std::ostringstream line;line<<std::setprecision(17);
        line<<"MELONX_GPU_STAGES {\"v\":1,\"id\":"<<capture->id<<",\"pool\":"<<capture->pool->poolId<<",\"frame\":"<<capture->frame<<",\"completedNs\":"<<completed
            <<",\"sampling\":\"complete_epoch\",\"inverseProbability\":"<<capture->pool->inverseProbability<<",\"encoders\":["<<capture->encoders[0]<<","<<capture->encoders[1]<<","<<capture->encoders[2]<<"],\"sampledEncoders\":"<<capture->sampledEncoders
            <<",\"unavailable\":"<<capture->unavailable<<",\"gpuError\":"<<(!success?1:0)
            <<",\"gpuBufferStartSeconds\":"<<buffer.GPUStartTime<<",\"gpuBufferEndSeconds\":"<<buffer.GPUEndTime
            <<",\"vertexTicks\":"<<durations.vertexTicks<<",\"fragmentTicks\":"<<durations.fragmentTicks<<",\"computeTicks\":"<<durations.computeTicks<<",\"blitTicks\":"<<durations.blitTicks<<",\"emptyStages\":"<<durations.emptyStages<<",\"unionTicks\":"<<durations.unionTicks
            <<",\"nsPerGpuTick\":";if(scale)line<<*scale;else line<<"null";
        auto clock=[&](const char* name,const GPUStageClock& c){line<<",\""<<name<<"\":["<<c.hostMidNs<<","<<c.hostSpanNs<<","<<c.cpuTicks<<","<<c.gpuTicks<<"]";};
        clock("clockBefore",capture->before);clock("clockAfter",after);clock("calibrationBefore",calibration.before);
        line<<",\"encoderKinds\":[";for(unsigned i=0;i<capture->sampledEncoders;++i){if(i)line<<",";line<<unsigned(capture->kinds[i]);}line<<"]";
        line<<",\"unmeasuredEncoders\":["<<capture->unmeasured[0]<<","<<capture->unmeasured[1]<<","<<capture->unmeasured[2]<<"]";
        line<<",\"encodedDraw\":[";for(unsigned i=0;i<capture->sampledEncoders;++i){if(i)line<<",";line<<(capture->draws[i]?1:0);}line<<"]";
        line<<",\"graphicsWork\":[";
        for(unsigned i=0;i<capture->sampledEncoders;++i) {
            if(i)line<<",";
            if(capture->kinds[i]!=0){line<<"null";continue;}
            const auto& w=capture->drawWork[i];const auto& e=capture->renderExtent[i];
            line<<"["<<w.vertexProgram<<","<<w.fragmentProgram<<","<<w.sequenceHash<<","<<w.draws<<","<<w.inputElements<<","<<w.indexedDraws<<","<<w.unknownDraws<<","<<(w.mixedPrograms?1:0)<<","<<(w.overflow?1:0)<<","<<e[0]<<","<<e[1]<<","<<e[2]<<"]";
        }
        line<<"]";
        line<<",\"computeWork\":[";for(unsigned i=0;i<capture->sampledEncoders;++i){if(i)line<<",";const auto& w=capture->computeWork[i];line<<"["<<w.firstProgram<<","<<w.sequenceHash<<","<<w.dispatches<<","<<w.groups<<","<<w.invocations<<","<<w.indirect<<","<<(w.mixedPrograms?1:0)<<","<<(w.overflow?1:0)<<","<<capture->fenceWaits[i]<<","<<capture->fenceHash[i]<<"]";}line<<"]";
        line<<",\"timestamps\":[";for(unsigned i=0;i<4*capture->sampledEncoders;++i){if(i)line<<",";line<<ticks[i];}line<<"]}";
        fprintf(stderr,"%s\n",line.str().c_str());
        // Keep the epoch slot until it is sealed and all its buffers complete.
        capture->pool->complete(capture->epoch);capture->completed=true;
    }
}
}

#endif
