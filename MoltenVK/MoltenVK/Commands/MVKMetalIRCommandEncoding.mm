#include "MVKReplayTrace.h"
#include "MVKMetalIRCommandEncoding.h"
#include "MVKCommandBuffer.h"
#include "MVKMetalIR.h"

void MVKMetalIRCommandEncoding::reset() {
	_arena = {};
	_runtime[0] = {};
	_runtime[1] = {};
	_rawRuntime[0] = {};
	_rawRuntime[1] = {};
	_draw = {};
}

MVKMetalIRCommandEncoding::BufferBinding MVKMetalIRCommandEncoding::copyBytes(const void* bytes, NSUInteger length) {
	mvkreplay::Timer trace(mvkreplay::ParameterCopy);
	assert(length);
	auto& arena = _arena;
	// Preserve the allocator's power-of-two size and alignment contract. Each
	// update gets new bytes; no in-flight draw can observe a later update.
	NSUInteger sliceSize = NSUInteger(1) << mvkPowerOfTwoExponent(std::max(length, NSUInteger(_encoder.getMetalFeatures().mtlBufferAlignment)));
	NSUInteger offset = (arena.nextOffset + sliceSize - 1) & ~(sliceSize - 1);
	if (!arena.allocation || offset > arena.allocation->_length || sliceSize > arena.allocation->_length - offset) {
		arena.allocation = _encoder.getTempMTLBuffer(std::max(NSUInteger(16 * 1024), sliceSize));
		arena.contents = static_cast<uint8_t*>(arena.allocation->getContents());
		arena.gpuAddress = arena.allocation->_mtlBuffer.gpuAddress + arena.allocation->_offset;
		offset = 0;
	}
	memcpy(arena.contents + offset, bytes, length);
	mvkreplay::runtimeParameterBytes(length);
	arena.nextOffset = offset + sliceSize;
	return { arena.allocation->_mtlBuffer, arena.gpuAddress + offset };
}

void MVKMetalIRCommandEncoding::cacheRuntimeData(bool compute, const void* bytes, uint32_t size) {
	auto& state = _runtime[compute ? 1 : 0];
	assert(size <= sizeof(state.bytes));
	if (state.cpuData && state.byteSize == size && !memcmp(state.bytes, bytes, size)) return;
	mvkreplay::directRuntimeUpload();
	state.binding = copyBytes(bytes, size);
	memcpy(state.bytes, bytes, size);
	state.byteSize = size;
	state.cpuData = true;
}

MVKMetalIRCommandEncoding::BufferBinding MVKMetalIRCommandEncoding::runtimeBinding(bool compute) const {
	return _runtime[compute ? 1 : 0].binding;
}

void MVKMetalIRCommandEncoding::cacheRawRuntimeData(bool compute, const void* bytes, uint32_t size) {
    auto& state = _rawRuntime[compute ? 1 : 0];
    assert(size <= sizeof(state.bytes));
    if (state.cpuData && state.byteSize == size && !memcmp(state.bytes, bytes, size)) return;
    mvkreplay::directRuntimeUpload();
    state.binding = copyBytes(bytes, size);
    memcpy(state.bytes, bytes, size);
    state.byteSize = size;
    state.cpuData = true;
}

MVKMetalIRCommandEncoding::BufferBinding MVKMetalIRCommandEncoding::rawRuntimeBinding(bool compute) const {
    return _rawRuntime[compute ? 1 : 0].binding;
}

void MVKMetalIRCommandEncoding::prepareDraw(const MVKMetalIRMetadata* artifact, const mvkir::DirectDraw& draw) {
    if (!artifact || !artifact->runtimeFlags) return;
    if (artifact->runtimeFlags & MVK_METAL_IR_DRAW_BASES)
        cacheRawRuntimeData(false, &draw.mesa.firstVertex, 8);
    if (artifact->runtimeFlags & MVK_METAL_IR_RUNTIME_DATA) {
        // Retain the established static-field offsets, including point lowering.
        auto data = draw.mesa;
        data.firstVertex = data.baseInstance = 0;
        cacheRuntimeData(false, &data, sizeof(data));
    }
    _draw = { draw.metal, nil, 0, draw.indexType };
}

MVKMetalIRCommandEncoding::RuntimeBatch MVKMetalIRCommandEncoding::prepareIndirectDraws(
    const MVKMetalIRMetadata* artifact, uint32_t count, bool indexed) {
    mvkreplay::Timer trace(mvkreplay::IndirectParameters);
    if (artifact && count) mvkreplay::indirectRuntime(count, 0, false, false);
    return {artifact && count, indexed, artifact ? artifact->runtimeFlags : 0};
}

void MVKMetalIRCommandEncoding::selectIndirectDraw(const RuntimeBatch& batch, uint32_t drawId,
    id<MTLBuffer> arguments, NSUInteger offset, uint16_t indexType) {
    if (!batch.active) return;
    if (batch.runtimeFlags & MVK_METAL_IR_DRAW_BASES) {
        auto& raw = _rawRuntime[0];
        raw.binding = {arguments, arguments.gpuAddress + offset + (batch.indexed ? 12 : 8)};
        raw.cpuData = false;
        raw.byteSize = 8;
    }
    if (batch.runtimeFlags & MVK_METAL_IR_RUNTIME_DATA) {
        mvkir::VertexData data{};
        data.isIndexedDraw = batch.indexed;
        data.drawId = drawId;
        cacheRuntimeData(false, &data, sizeof(data));
    }
    _draw = { {}, arguments, offset, indexType };
}

void MVKMetalIRCommandEncoding::prepareDispatch(const MVKMetalIRMetadata* artifact, const mvkir::ComputeData& data) {
    if (!artifact) return;
    if (artifact->runtimeFlags & MVK_METAL_IR_DISPATCH_GROUPS)
        cacheRawRuntimeData(true, data.groupCount, sizeof(data.groupCount));
    if (artifact->runtimeFlags & MVK_METAL_IR_RUNTIME_DATA) {
        auto fixed = data;
        memset(fixed.groupCount, 0, sizeof(fixed.groupCount));
        cacheRuntimeData(true, &fixed, sizeof(fixed));
    }
}

void MVKMetalIRCommandEncoding::prepareIndirectDispatch(const MVKMetalIRMetadata* artifact,
    id<MTLBuffer> arguments, NSUInteger offset) {
    if (!artifact) return;
    mvkreplay::indirectRuntime(1, 0, false, true);
    if (artifact->runtimeFlags & MVK_METAL_IR_DISPATCH_GROUPS) {
        auto& raw = _rawRuntime[1];
        raw.binding = {arguments, arguments.gpuAddress + offset};
        raw.cpuData = false;
        raw.byteSize = 12;
    }
    if (artifact->runtimeFlags & MVK_METAL_IR_RUNTIME_DATA) {
        const mvkir::ComputeData data{};
        cacheRuntimeData(true, &data, sizeof(data));
    }
}
