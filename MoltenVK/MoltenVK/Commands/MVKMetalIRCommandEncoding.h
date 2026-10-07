#pragma once
#include "MVKMetalIRRuntimeData.h"
#import <Metal/Metal.h>

class MVKCommandEncoder;
struct MVKMetalIRArtifact;
class MVKMTLBufferAllocation;

// Owns IR parameter preparation only. Compilation, descriptor translation and
// resource binding stay independent. Allocations are retained by the ordinary
// command-buffer completion pool, so this state holds no separate Metal owners.
class MVKMetalIRCommandEncoding {
public:
    struct BufferBinding {
        id<MTLBuffer> buffer = nil;
        uint64_t gpuAddress = 0;
    };
    struct RuntimeBatch {
        bool active = false;
        bool indexed = false;
        uint32_t runtimeFlags = 0;
    };
    struct DrawBinding {
        mvkir::DrawArguments arguments{};
        id<MTLBuffer> indirectBuffer = nil;
        NSUInteger indirectOffset = 0;
        uint16_t indexType = 0;
    };

    explicit MVKMetalIRCommandEncoding(MVKCommandEncoder& encoder) : _encoder(encoder) {}
    void reset();
    BufferBinding copyBytes(const void* bytes, NSUInteger length);
    void prepareDraw(const MVKMetalIRArtifact* artifact, const mvkir::DirectDraw& draw);
    RuntimeBatch prepareIndirectDraws(const MVKMetalIRArtifact* artifact, uint32_t count, bool indexed);
    void selectIndirectDraw(const RuntimeBatch& batch, uint32_t drawId,
        id<MTLBuffer> arguments, NSUInteger offset, uint16_t indexType);
    void prepareDispatch(const MVKMetalIRArtifact* artifact, const mvkir::ComputeData& data);
    void prepareIndirectDispatch(const MVKMetalIRArtifact* artifact, id<MTLBuffer> arguments, NSUInteger offset);
    BufferBinding runtimeBinding(bool compute) const;
    BufferBinding rawRuntimeBinding(bool compute) const;
    const DrawBinding& drawBinding() const { return _draw; }

private:
    void cacheRuntimeData(bool compute, const void* bytes, uint32_t size);
    void cacheRawRuntimeData(bool compute, const void* bytes, uint32_t size);
    MVKCommandEncoder& _encoder;
    struct Arena {
        const MVKMTLBufferAllocation* allocation = nullptr;
        uint8_t* contents = nullptr;
        uint64_t gpuAddress = 0;
        NSUInteger nextOffset = 0;
    } _arena;
    struct RuntimeState {
        uint8_t bytes[sizeof(mvkir::VertexData)] = {};
        uint32_t byteSize = 0;
        bool cpuData = false;
        BufferBinding binding{};
    } _runtime[2], _rawRuntime[2]; // vertex, compute
    DrawBinding _draw;
};
