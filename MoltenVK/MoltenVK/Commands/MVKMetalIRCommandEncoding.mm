#include "MVKReplayTrace.h"
#include "MVKMetalIRCommandEncoding.h"
#include "MVKCommandBuffer.h"
#include "MVKMetalIR.h"

void MVKMetalIRCommandEncoding::reset() {
	_arena = {};
	_runtime[0] = {};
	_runtime[1] = {};
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

void MVKMetalIRCommandEncoding::prepareDraw(const MVKMetalIRArtifact* artifact, const mvkir::DirectDraw& draw) {
	if (!artifact || !artifact->runtimeFlags) return;
	if (artifact->runtimeFlags & MVK_METAL_IR_RUNTIME_DATA) cacheRuntimeData(false, &draw.mesa, sizeof(draw.mesa));
	_draw = { draw.metal, nil, 0, draw.indexType };
}

MVKMetalIRCommandEncoding::RuntimeBatch MVKMetalIRCommandEncoding::prepareIndirectDraws(
	const MVKMetalIRArtifact* artifact, id<MTLBuffer> arguments, NSUInteger offset,
	NSUInteger stride, uint32_t count, bool indexed) {
	mvkreplay::Timer trace(mvkreplay::IndirectParameters);
	if (artifact && count) mvkreplay::indirectInvocation(count,false);
	if (!artifact || !(artifact->runtimeFlags & MVK_METAL_IR_RUNTIME_DATA) || !count) return {};
	NSUInteger alignment = std::max(NSUInteger(_encoder.getMetalFeatures().mtlBufferAlignment), NSUInteger(16));
	NSUInteger runtimeStride = (sizeof(mvkir::VertexData) + alignment - 1) / alignment * alignment;
	assert(count <= NSUIntegerMax / runtimeStride);
	const auto* allocation = _encoder.getTempMTLBuffer(runtimeStride * count);
	mvkreplay::indirectRuntime(count,runtimeStride*count,_encoder._mtlRenderEncoder!=nil,false);
	auto* contents = static_cast<uint8_t*>(allocation->getContents());
	for (uint32_t index = 0; index < count; ++index) {
		mvkir::VertexData data{};
		data.isIndexedDraw = indexed;
		data.drawId = index;
		memcpy(contents + index * runtimeStride, &data, sizeof(data));
	}
	// Never read an indirect buffer on the CPU. Copy its GPU-written base values
	// in one blit encoder before the draw loop, preserving command order.
	_encoder.encodeStoreActions(true);
	auto encoder = _encoder.getMTLBlitEncoder(kMVKCommandUseCopyBuffer);
	for (uint32_t index = 0; index < count; ++index) {
		[encoder copyFromBuffer:arguments sourceOffset:offset + index * stride + (indexed ? 12 : 8)
					 toBuffer:allocation->_mtlBuffer destinationOffset:allocation->_offset + index * runtimeStride size:8];
	}
	return { allocation->_mtlBuffer, allocation->_offset, runtimeStride };
}

void MVKMetalIRCommandEncoding::selectIndirectDraw(const RuntimeBatch& batch, uint32_t drawId,
	id<MTLBuffer> arguments, NSUInteger offset, uint16_t indexType) {
	if (batch.buffer) {
		auto& state = _runtime[0];
		state.binding = { batch.buffer, batch.buffer.gpuAddress + batch.offset + drawId * batch.stride };
		state.cpuData = false;
		state.byteSize = sizeof(mvkir::VertexData);
	}
	_draw = { {}, arguments, offset, indexType };
}

void MVKMetalIRCommandEncoding::prepareDispatch(const MVKMetalIRArtifact* artifact, const mvkir::ComputeData& data) {
	if (artifact && (artifact->runtimeFlags & MVK_METAL_IR_RUNTIME_DATA)) cacheRuntimeData(true, &data, sizeof(data));
}

void MVKMetalIRCommandEncoding::prepareIndirectDispatch(const MVKMetalIRArtifact* artifact,
	id<MTLBuffer> arguments, NSUInteger offset) {
	if (artifact) mvkreplay::indirectInvocation(1,true);
	if (!artifact || !(artifact->runtimeFlags & MVK_METAL_IR_RUNTIME_DATA)) return;
	const auto* allocation = _encoder.getTempMTLBuffer(sizeof(mvkir::ComputeData));
	mvkreplay::indirectRuntime(0,sizeof(mvkir::ComputeData),_encoder._mtlRenderEncoder!=nil,true);
	mvkir::ComputeData data{};
	memcpy(allocation->getContents(), &data, sizeof(data));
	auto encoder = _encoder.getMTLBlitEncoder(kMVKCommandUseCopyBuffer);
	[encoder copyFromBuffer:arguments sourceOffset:offset toBuffer:allocation->_mtlBuffer
			 destinationOffset:allocation->_offset size:12];
	auto& state = _runtime[1];
	state.binding = { allocation->_mtlBuffer, allocation->_mtlBuffer.gpuAddress + allocation->_offset };
	state.cpuData = false;
	state.byteSize = sizeof(data);
}
