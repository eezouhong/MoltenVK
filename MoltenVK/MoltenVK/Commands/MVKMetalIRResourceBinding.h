#pragma once
#include "MVKCommandBuffer.h"
#include "MVKMetalIR.h"
#include "MVKPipeline.h"
#include "MVKReplayTrace.h"

// IR root/table/runtime binding stays separate from the common Vulkan bind
// script. The caller supplies that shared residency step at the same point.
// This template callback is inlined: no erased function or heap allocation.
template <typename EncodeResidency>
inline void mvkBindMetalIRResources(id<MTLCommandEncoder> encoder,
                               MVKCommandEncoder& mvkEncoder,
                               const MVKVulkanCommonEncoderState& common,
                               const MVKPipelineStageResourceInfo& resources,
                               const uint8_t* pushConstants,
                               MVKShaderStage vkStage,
                               MVKResourceUsageStages useResourceStage,
                               MVKStageResourceBits& exists,
                               MVKStageResourceBindings& bindings,
                               const MVKResourceBinder& RESTRICT binder,
                               mvkreplay::BindingTrace& bindingTrace,
                               const EncodeResidency& encodeResidency) {
	mvkreplay::Timer replayTrace(mvkreplay::IRRootBinding);
	mvkreplay::NativePhaseTrace chainTrace(mvkreplay::BindingGroup::IRRootBinding);
	const auto& artifact = *resources.metalIR;
	auto& cached = bindings.metalIRArguments;
	MVKMetalSharedCommandEncoderState& shared = mvkEncoder.getState().mtlShared();
	if (!bindings.metalIR) {
		exists.descriptorSetData.reset();
		bindings.metalIR = true;
		cached.reset();
	}
	if (cached.stage != vkStage) {
		cached.reset();
		cached.stage = vkStage;
	}
	const uint32_t rootPayloadFlags = MVK_METAL_IR_RUNTIME_DATA | MVK_METAL_IR_DRAW_PARAMETERS |
		MVK_METAL_IR_DRAW_BASES | MVK_METAL_IR_DISPATCH_GROUPS;
	if (artifact.usedSets && artifact.setCount <= kMVKMaxDescriptorSetCount &&
		!(artifact.usedSets & (artifact.usedSets - 1)) &&
		!artifact.usesPushConstants && !(artifact.runtimeFlags & rootPayloadFlags)) {
		const uint32_t idx = __builtin_ctzll(artifact.usedSets);
		MVKDescriptorSet* set = idx < artifact.setCount ? common._descriptorSets[idx] : nullptr;
		const uint32_t rootStart = set ? set->metalIRRootOffset() : UINT32_MAX;
		if (rootStart != UINT32_MAX) {
			// End the stage's root at the last pair. Its unused push slot then
			// reads the terminal zero regardless of the stage's trimmed set count.
			const uint32_t rootOffset = rootStart + (kMVKMaxDescriptorSetCount - artifact.setCount) * 2 * sizeof(uint64_t);
			const bool refreshAddress = !exists.descriptorSetData.get(idx) || !cached.descriptorSetBases[idx];
			if (!exists.descriptorSetData.get(idx)) {
				bindings.descriptorSetResourceUse[idx].resizeAndClear(set->layout->bindings().size());
				exists.descriptorSetData.set(idx);
			}
			// Refresh the address cache even for a direct root: a later ordinary
			// root can reuse descriptorSetData without observing this invalidation.
			if (refreshAddress)
				cached.descriptorSetBases[idx] = set->metalIRTableAddress;
			bindingTrace.checkpoint();
			encodeResidency();
			bindingTrace.checkpoint();
			auto& bound = bindings.buffers[2];
			if (!exists.buffers.get(2) || bound.buffer != set->gpuBufferObject) {
				binder.setBuffer(encoder, set->gpuBufferObject, rootOffset, 2);
				exists.buffers.set(2);
				bound = {set->gpuBufferObject, rootOffset};
			} else if (bound.offset != rootOffset) {
				binder.setBufferOffset(encoder, rootOffset, 2);
				bound.offset = rootOffset;
			}
			// Metal binding retains/resides the same buffer containing both the
			// root and its table. Descriptor resources still use the common script.
			cached.rootArtifact = nullptr; // Force exact ABI refresh on bytes fallback.
			return;
		}
	}
	const bool usesRuntime = artifact.runtimeFlags & MVK_METAL_IR_RUNTIME_DATA;
	const auto runtime = usesRuntime
		? mvkEncoder.metalIR().runtimeBinding(vkStage == kMVKShaderStageCompute)
		: MVKMetalIRCommandEncoding::BufferBinding{};
	const bool usesRaw = artifact.runtimeFlags & (MVK_METAL_IR_DRAW_BASES | MVK_METAL_IR_DISPATCH_GROUPS);
	const auto raw = usesRaw ? mvkEncoder.metalIR().rawRuntimeBinding(vkStage == kMVKShaderStageCompute)
		: MVKMetalIRCommandEncoding::BufferBinding{};
	const uint32_t argumentCount = artifact.setCount * 2 + (artifact.pushConstantSize ? 1 : 0) + (usesRuntime ? 1 : 0) + (usesRaw ? 1 : 0);
	const uint32_t argumentBytes = argumentCount * sizeof(uint64_t);
	// Vulkan pipeline resources outlive their encoded commands. This key is used
	// only within one Metal encoder/stage, and reset with that encoder's state.
	// Different shaders can use the same root payload. Compare its exact ABI
	// and usage on a pipeline switch; shader identity alone need not rebuild it.
	const uint32_t rootRuntimeFlags = artifact.runtimeFlags &
		(MVK_METAL_IR_RUNTIME_DATA | MVK_METAL_IR_DRAW_BASES | MVK_METAL_IR_DISPATCH_GROUPS);
	const bool rootLayoutChanged = cached.rootArtifact != &artifact &&
		(!cached.rootArtifact || cached.rootUsedSets != artifact.usedSets ||
		 cached.rootSetCount != artifact.setCount ||
		 cached.rootPushConstantSize != artifact.pushConstantSize ||
		 cached.rootUsesPushConstants != artifact.usesPushConstants ||
		 cached.rootRuntimeFlags != rootRuntimeFlags);
	const bool needsRoot = rootLayoutChanged ||
		(artifact.usedSets & ~uint64_t(exists.descriptorSetData.bits())) ||
		(artifact.usesPushConstants && artifact.pushConstantSize && cached.pushConstantSize != artifact.pushConstantSize) ||
		(usesRuntime && (cached.runtimeAddress != runtime.gpuAddress || cached.runtimeBuffer != runtime.buffer)) ||
		(usesRaw && (cached.rawRuntimeAddress != raw.gpuAddress || cached.rawRuntimeBuffer != raw.buffer)) ||
		(argumentBytes && (!exists.buffers.get(2) || bindings.buffers[2] != MVKStageResourceBindings::MetalIRRootBuffer()));
	uint64_t args[kMVKMaxDescriptorSetCount * 2 + 3];
	if (needsRoot) {
		memset(args, 0, sizeof(args));
		for (uint32_t idx = 0; idx < artifact.setCount; ++idx) {
			if (!(artifact.usedSets & (1ull << idx))) continue;
			MVKDescriptorSet* set = common._descriptorSets[idx];
			if (!set || !set->gpuBufferObject) continue;
			const auto* layout = set->layout;
			bool refreshAddress = !exists.descriptorSetData.get(idx) || !cached.descriptorSetBases[idx];
			if (!exists.descriptorSetData.get(idx)) {
				bindings.descriptorSetResourceUse[idx].resizeAndClear(layout->bindings().size());
				exists.descriptorSetData.set(idx);
				// Residency lasts for the encoder, just like the descriptor resources
				// tracked by executeBindOps. Rebinding the set invalidates this bit.
				shared._useResource.add(set->gpuBufferObject, useResourceStage, false);
			}
			if (refreshAddress) {
				cached.descriptorSetBases[idx] = set->metalIRTableAddress;
			}
			// Descriptor contents may change without changing their allocation. Keep
			// reading the table itself on the GPU; only reuse its encoder-local address.
			uint64_t base = cached.descriptorSetBases[idx];
			args[idx * 2] = base;
			args[idx * 2 + 1] = base;
		}
	}
	bindingTrace.checkpoint();
	encodeResidency();
	bindingTrace.checkpoint();
	if (needsRoot) {
		if (artifact.pushConstantSize && artifact.usesPushConstants) {
			if (cached.pushConstantSize != artifact.pushConstantSize) {
				const auto slice = mvkEncoder.metalIR().copyBytes(pushConstants, artifact.pushConstantSize);
				cached.pushConstantAddress = slice.gpuAddress;
				cached.pushConstantSize = artifact.pushConstantSize;
				// A chunk can serve many draws. Residency is already registered for
				// this stage until the Metal encoder (and this cache) is reset.
				if (cached.pushConstantBuffer != slice.buffer) {
					shared._useResource.add(slice.buffer, useResourceStage, false);
					cached.pushConstantBuffer = slice.buffer;
				}
			}
			args[artifact.setCount * 2] = cached.pushConstantAddress;
		}
		// Keep the compiler's full root-table ABI. An unused stage gets a stable
		// zero push pointer, so unrelated updates neither allocate nor rebind it.
		uint32_t runtimeIndex = artifact.setCount * 2 + (artifact.pushConstantSize ? 1 : 0);
		if (usesRuntime) {
			assert(runtime.buffer && runtime.gpuAddress);
			args[runtimeIndex] = runtime.gpuAddress;
			if (cached.runtimeBuffer != runtime.buffer) {
				shared._useResource.add(runtime.buffer, useResourceStage, false);
				cached.runtimeBuffer = runtime.buffer;
			}
		}
		if (usesRaw) {
			assert(raw.buffer && raw.gpuAddress);
			args[runtimeIndex + (usesRuntime ? 1 : 0)] = raw.gpuAddress;
			if (cached.rawRuntimeBuffer != raw.buffer) {
				shared._useResource.add(raw.buffer, useResourceStage, false);
				cached.rawRuntimeBuffer = raw.buffer;
			}
		}
		if (argumentBytes && (!exists.buffers.get(2) ||
			bindings.buffers[2] != MVKStageResourceBindings::MetalIRRootBuffer() ||
			cached.argumentBytes != argumentBytes || memcmp(cached.arguments, args, argumentBytes))) {
			binder.setBytes(encoder, args, argumentBytes, 2);
			memcpy(cached.arguments, args, argumentBytes);
			cached.argumentBytes = argumentBytes;
			exists.buffers.set(2);
			bindings.buffers[2] = MVKStageResourceBindings::MetalIRRootBuffer();
		}
		cached.rootUsedSets = artifact.usedSets;
		cached.rootSetCount = artifact.setCount;
		cached.rootPushConstantSize = artifact.pushConstantSize;
		cached.rootUsesPushConstants = artifact.usesPushConstants;
		cached.rootRuntimeFlags = rootRuntimeFlags;
		cached.runtimeAddress = runtime.gpuAddress;
		cached.rawRuntimeAddress = raw.gpuAddress;
	}
	cached.rootArtifact = &artifact;
	if (artifact.runtimeFlags & MVK_METAL_IR_DRAW_PARAMETERS) {
		assert(vkStage == kMVKShaderStageVertex);
		const auto& draw = mvkEncoder.metalIR().drawBinding();
		bool sameArguments = cached.drawArgumentsValid &&
			cached.drawIndirectBuffer == draw.indirectBuffer &&
			(draw.indirectBuffer ? cached.drawIndirectOffset == draw.indirectOffset :
			 !memcmp(cached.drawArguments, draw.arguments.words, sizeof(cached.drawArguments)));
		if (!sameArguments || !exists.buffers.get(4) || bindings.buffers[4] != MVKStageResourceBindings::MetalIRDrawBuffer()) {
			if (draw.indirectBuffer) {
				binder.setBuffer(encoder, draw.indirectBuffer, draw.indirectOffset, 4);
			} else binder.setBytes(encoder, &draw.arguments, sizeof(draw.arguments), 4);
			memcpy(cached.drawArguments, draw.arguments.words, sizeof(cached.drawArguments));
			cached.drawIndirectBuffer = draw.indirectBuffer;
			cached.drawIndirectOffset = draw.indirectOffset;
			cached.drawArgumentsValid = true;
			exists.buffers.set(4);
			bindings.buffers[4] = MVKStageResourceBindings::MetalIRDrawBuffer();
		}
		if (!cached.drawIndexTypeValid || cached.drawIndexType != draw.indexType ||
			!exists.buffers.get(5) || bindings.buffers[5] != MVKStageResourceBindings::MetalIRDrawInfoBuffer()) {
			binder.setBytes(encoder, &draw.indexType, sizeof(draw.indexType), 5);
			cached.drawIndexType = draw.indexType;
			cached.drawIndexTypeValid = true;
			exists.buffers.set(5);
			bindings.buffers[5] = MVKStageResourceBindings::MetalIRDrawInfoBuffer();
		}
	}
}
