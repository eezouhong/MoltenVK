// Opt-in stage timing on existing encoders; never inserts GPU work.
#pragma once
#include "MVKReplayConfig.h"
#import <Metal/Metal.h>
#include <memory>
#include "MVKReplayDrawWork.h"

namespace mvkreplay {
class GPUStagePool;
class GPUStageCapture;
#if MVK_REPLAY_TRACE
std::shared_ptr<GPUStagePool> createGPUStagePool(id<MTLDevice> device);
std::shared_ptr<GPUStageCapture> beginGPUStages(const std::shared_ptr<GPUStagePool>& pool,
                                             id<MTLCommandBuffer> buffer, uint64_t frame);
bool attachGPUStages(id<MTLCommandBuffer> buffer, MTLRenderPassDescriptor* pass);
MTLComputePassDescriptor* computeGPUStagePass(id<MTLCommandBuffer> buffer, MTLDispatchType dispatch);
MTLBlitPassDescriptor* blitGPUStagePass(id<MTLCommandBuffer> buffer);
void sealGPUStageEpoch(uint64_t frame);
void noteUnmeasuredGPUStage(id<MTLCommandBuffer> buffer,unsigned kind);
bool gpuStageTracingEnabled();
void noteGPUStageDraw(id<MTLCommandBuffer> buffer);
void noteGPUStageRenderWork(id<MTLCommandBuffer> buffer,const GPUDrawWork& work);
void noteGPUStageDispatch(id<MTLCommandBuffer> buffer,uint64_t program,uint64_t x,uint64_t y,uint64_t z,
                          uint64_t tx,uint64_t ty,uint64_t tz,bool indirect);
void noteGPUStageFenceWait(id<MTLCommandBuffer> buffer,unsigned source,unsigned target,uint64_t index);
void finishGPUStages(const std::shared_ptr<GPUStageCapture>& capture, id<MTLCommandBuffer> buffer);
#else
inline std::shared_ptr<GPUStagePool> createGPUStagePool(id<MTLDevice>) { return {}; }
inline std::shared_ptr<GPUStageCapture> beginGPUStages(const std::shared_ptr<GPUStagePool>&,id<MTLCommandBuffer>,uint64_t) { return {}; }
inline bool attachGPUStages(id<MTLCommandBuffer>,MTLRenderPassDescriptor*) { return false; }
inline MTLComputePassDescriptor* computeGPUStagePass(id<MTLCommandBuffer>,MTLDispatchType) { return nullptr; }
inline MTLBlitPassDescriptor* blitGPUStagePass(id<MTLCommandBuffer>) { return nullptr; }
inline void sealGPUStageEpoch(uint64_t) {}
inline void noteUnmeasuredGPUStage(id<MTLCommandBuffer>,unsigned) {}
inline bool gpuStageTracingEnabled() { return false; }
inline void noteGPUStageDraw(id<MTLCommandBuffer>) {}
inline void noteGPUStageRenderWork(id<MTLCommandBuffer>,const GPUDrawWork&) {}
inline void noteGPUStageDispatch(id<MTLCommandBuffer>,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,uint64_t,bool) {}
inline void noteGPUStageFenceWait(id<MTLCommandBuffer>,unsigned,unsigned,uint64_t) {}
inline void finishGPUStages(const std::shared_ptr<GPUStageCapture>&,id<MTLCommandBuffer>) {}
#endif

}
