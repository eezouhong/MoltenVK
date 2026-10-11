#pragma once
#include "MVKMetalIRCommandEncoding.h"

// Per-stage metadata controls invocation. This facade owns no arena, Metal
// object or completion pool; the encoder's existing IR storage owns everything.
class MVKShaderCommandHooks {
    MVKMetalIRCommandEncoding& _encoding;
public:
    using IndirectBatch = MVKMetalIRCommandEncoding::RuntimeBatch;
    explicit MVKShaderCommandHooks(MVKMetalIRCommandEncoding& encoding) : _encoding(encoding) {}
    void prepareDraw(const MVKMetalIRMetadata* metadata, const mvkir::DirectDraw& draw) { _encoding.prepareDraw(metadata, draw); }
    IndirectBatch prepareIndirectDraws(const MVKMetalIRMetadata* metadata, uint32_t count, bool indexed) { return _encoding.prepareIndirectDraws(metadata, count, indexed); }
    void selectIndirectDraw(const IndirectBatch& batch, uint32_t drawId, id<MTLBuffer> buffer, NSUInteger offset, uint16_t indexType) { _encoding.selectIndirectDraw(batch, drawId, buffer, offset, indexType); }
    void prepareDispatch(const MVKMetalIRMetadata* metadata, const mvkir::ComputeData& data) { _encoding.prepareDispatch(metadata, data); }
    void prepareIndirectDispatch(const MVKMetalIRMetadata* metadata, id<MTLBuffer> buffer, NSUInteger offset) { _encoding.prepareIndirectDispatch(metadata, buffer, offset); }
};
