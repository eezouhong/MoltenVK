// Opt-in stage timing on existing encoders; never inserts GPU work.
#pragma once
#import <Metal/Metal.h>
#include <memory>

namespace mvkreplay {
class GPUStagePool;
class GPUStageCapture;
std::shared_ptr<GPUStagePool> createGPUStagePool(id<MTLDevice> device);
std::shared_ptr<GPUStageCapture> beginGPUStages(const std::shared_ptr<GPUStagePool>& pool,
                                             id<MTLCommandBuffer> buffer, uint64_t frame);
void attachGPUStages(id<MTLCommandBuffer> buffer, MTLRenderPassDescriptor* pass);
MTLComputePassDescriptor* computeGPUStagePass(id<MTLCommandBuffer> buffer, MTLDispatchType dispatch);
MTLBlitPassDescriptor* blitGPUStagePass(id<MTLCommandBuffer> buffer);
void sealGPUStageEpoch(uint64_t frame);
void noteUnmeasuredGPUStage(id<MTLCommandBuffer> buffer,unsigned kind);
bool gpuStageTracingEnabled();
void noteGPUStageDraw(id<MTLCommandBuffer> buffer);
void noteGPUStageDispatch(id<MTLCommandBuffer> buffer,uint64_t program,uint64_t x,uint64_t y,uint64_t z,
                          uint64_t tx,uint64_t ty,uint64_t tz,bool indirect);
void noteGPUStageFenceWait(id<MTLCommandBuffer> buffer,unsigned source,unsigned target,uint64_t index);
void finishGPUStages(const std::shared_ptr<GPUStageCapture>& capture, id<MTLCommandBuffer> buffer);
}
