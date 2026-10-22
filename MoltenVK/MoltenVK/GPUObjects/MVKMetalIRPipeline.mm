#include "MVKShaderBackend.h"
#include "MVKPipeline.h"
#include "MVKMetalIR.h"
#include "MVKShaderModule.h"
#include "MVKFoundation.h"
#include "MVKStrings.h"
#include "MTLRenderPipelineDescriptor+MoltenVK.h"
#include "mvk_datatypes.hpp"

// IR-specific Vulkan support, compile request and reflection handling. The
// generic pipeline owns the descriptor and keeps its original release points.
bool MVKGraphicsPipeline::addMetalIRShadersToPipeline(
    MTLRenderPipelineDescriptor* plDesc, const VkGraphicsPipelineCreateInfo* pCreateInfo,
    mvk::SPIRVToMSLConversionConfiguration& shaderConfig,
    const VkPipelineShaderStageCreateInfo* pVertexSS,
    const VkPipelineShaderStageCreateInfo* pFragmentSS, uint32_t viewMask) {
	uint32_t conflictingVertexBinding = UINT32_MAX;
	const auto& capabilities = getDevice()->shaderBackend().capabilities();
    const char* rejectionReason = capabilities.graphicsRejectionReason(isTessellationPipeline(), isMeshPipeline(),
        pCreateInfo->pRasterizationState->rasterizerDiscardEnable, mvkIsMultiview(viewMask));
    bool tryMetalIR = capabilities.supportsGraphics(isTessellationPipeline(), isMeshPipeline(),
        pCreateInfo->pRasterizationState->rasterizerDiscardEnable, mvkIsMultiview(viewMask));
	if (tryMetalIR && pCreateInfo->pVertexInputState) {
		for (const auto& binding : MVKArrayRef(pCreateInfo->pVertexInputState->pVertexBindingDescriptions,
		                                     pCreateInfo->pVertexInputState->vertexBindingDescriptionCount)) {
			if (!capabilities.supportsVertexBinding(getMetalBufferIndexForVertexAttributeBinding(binding.binding))) {
				tryMetalIR = false;
                rejectionReason = "vertex_binding_conflict";
				conflictingVertexBinding = binding.binding;
			}
		}
	}
	if (tryMetalIR && _isRasterizing && pCreateInfo->pMultisampleState && !capabilities.supportsSampleShading(pCreateInfo->pMultisampleState->sampleShadingEnable,
        pCreateInfo->pMultisampleState->minSampleShading)) {
        tryMetalIR = false;
        rejectionReason = "sample_shading";
    }
	if (!tryMetalIR) {
        mvkMetalIRRecordPipelineRejection(getDevice(), rejectionReason);
        setConfigurationResult(reportError(VK_ERROR_FEATURE_NOT_PRESENT,
            "MetalIR graphics pipeline rejected: topology=%u, polygon=%u, discard=%u, view_mask=0x%x, sample_shading=%u, min_sample_shading=%.3f, vertex_binding=%u, vertex_code=%016zx, fragment_code=%016zx; MSL fallback disabled.",
            pCreateInfo->pInputAssemblyState ? pCreateInfo->pInputAssemblyState->topology : UINT32_MAX,
            pCreateInfo->pRasterizationState->polygonMode,
            pCreateInfo->pRasterizationState->rasterizerDiscardEnable,
            viewMask,
            _isRasterizing && pCreateInfo->pMultisampleState ? pCreateInfo->pMultisampleState->sampleShadingEnable : 0,
            _isRasterizing && pCreateInfo->pMultisampleState ? pCreateInfo->pMultisampleState->minSampleShading : 0.0f,
            conflictingVertexBinding, _vertexModule->getKey().codeHash,
            _fragmentModule ? _fragmentModule->getKey().codeHash : 0));
        return false;
    }
	if (tryMetalIR) {
		uint32_t transforms = (shaderConfig.options.shouldFlipVertexY ? MVK_METAL_IR_FLIP_Y : 0) |
			(shaderConfig.options.shouldFixupClipSpace ? MVK_METAL_IR_CLIP_HALF_Z : 0);
        uint32_t rasterOptions = _isRasterizing && getPrimitiveTopologyClass() == MTLPrimitiveTopologyClassPoint
            ? MVK_METAL_IR_RENDERING_POINTS : 0;
        if (!_isRasterizing) rasterOptions |= MVK_METAL_IR_RASTERIZATION_DISABLED;
        auto vertexIR = getDevice()->shaderBackend().compileStage({this, _layout, _vertexModule, pVertexSS, transforms, rasterOptions, {}}).artifactOwner;
		if (vertexIR && _isRasterizing && getPrimitiveTopologyClass() == MTLPrimitiveTopologyClassPoint &&
            !capabilities.supportsPointSize(vertexIR->runtimeFlags)) {
            mvkMetalIRRecordPipelineRejection(getDevice(), "point_size_semantics");
			setConfigurationResult(reportError(VK_ERROR_FEATURE_NOT_PRESENT,
                "MetalIR point pipeline has no native point size or proven one-pixel default: vertex_code=%016zx, fragment_code=%016zx; MSL fallback disabled.",
				_vertexModule->getKey().codeHash, _fragmentModule ? _fragmentModule->getKey().codeHash : 0));
			return false;
		}
		if (vertexIR && (vertexIR->runtimeFlags & MVK_METAL_IR_DRAW_PARAMETERS) && pCreateInfo->pVertexInputState) {
			for (const auto& binding : MVKArrayRef(pCreateInfo->pVertexInputState->pVertexBindingDescriptions,
				pCreateInfo->pVertexInputState->vertexBindingDescriptionCount)) {
				uint32_t index = getMetalBufferIndexForVertexAttributeBinding(binding.binding);
				if (!capabilities.supportsRuntimeVertexBinding(index)) {
                    mvkMetalIRRecordPipelineRejection(getDevice(), "runtime_vertex_binding");
					setConfigurationResult(reportError(VK_ERROR_FEATURE_NOT_PRESENT, "MetalIR runtime draw buffer conflicts with vertex binding; MSL fallback disabled."));
					vertexIR.reset(); break;
				}
			}
		}
        // Vertex-only work still writes resources. A discarded fragment stage
        // is ignored by Vulkan and must not be compiled or encoded.
        auto fragmentIR = vertexIR && _isRasterizing && pFragmentSS ? getDevice()->shaderBackend().compileStage({this, _layout, _fragmentModule, pFragmentSS, 0, rasterOptions, {},
            !shaderConfig.options.mslOptions.enable_frag_depth_builtin,
            !shaderConfig.options.mslOptions.enable_frag_stencil_ref_builtin}).artifactOwner : nullptr;
        if (fragmentIR && getPrimitiveTopologyClass() == MTLPrimitiveTopologyClassPoint && fragmentIR->usesPointCoordinates &&
            !capabilities.supportsPointCoordinates(fragmentIR->runtimeFlags)) {
            mvkMetalIRRecordPipelineRejection(getDevice(), "point_coordinate_semantics");
			setConfigurationResult(reportError(VK_ERROR_FEATURE_NOT_PRESENT,
                "MetalIR point coordinates have no native rasterizer input; MSL fallback disabled."));
			return false;
		}
		if (vertexIR && (!_isRasterizing || !pFragmentSS || fragmentIR)) {
			_stageResources[kMVKShaderStageVertex].metalIR = vertexIR->metadata;
			_stageResources[kMVKShaderStageFragment].metalIR = fragmentIR ? fragmentIR->metadata : nullptr;
			plDesc.vertexFunction = vertexIR->function;
			plDesc.fragmentFunction = fragmentIR ? fragmentIR->function : nil;
			// Preserve the constructor's static discard/cull decision.
			plDesc.rasterizationEnabled = _isRasterizing;
			addVertexInputToShaderConversionConfig(shaderConfig, pCreateInfo);
			shaderConfig.markAllInterfaceVarsAndResourcesUsed();
			mvkPopulateMetalIRResidencyOperations(_layout,_stageResources[kMVKShaderStageVertex]);
			if(fragmentIR)mvkPopulateMetalIRResidencyOperations(_layout,_stageResources[kMVKShaderStageFragment]);
			for (auto& input : shaderConfig.shaderInputs)
				input.outIsUsedByShader = input.shaderVar.location < 32 && (vertexIR->vertexLocations & (1ull << input.shaderVar.location));
			if (!addVertexInputToPipeline(plDesc.vertexDescriptor, pCreateInfo->pVertexInputState, shaderConfig)) {
				_stageResources[kMVKShaderStageVertex].metalIR.reset();
				_stageResources[kMVKShaderStageFragment].metalIR.reset();
				return false;
			}
			addFragmentOutputToPipeline(plDesc, pCreateInfo);
			setMetalObjectLabel(plDesc, _layout->getDebugName());
			return true;
		}
		_stageResources[kMVKShaderStageVertex].metalIR.reset();
		_stageResources[kMVKShaderStageFragment].metalIR.reset();
        return false;
	}
	return false;
}

// A mesh shader occupies the vertex stage of this pipeline: its IR artifact
// supplies the vertex stage resources, which the encoder binds to the mesh stage.
bool MVKGraphicsPipeline::addMetalIRMeshShadersToPipeline(
    MTLMeshRenderPipelineDescriptor* plDesc, const VkGraphicsPipelineCreateInfo* pCreateInfo,
    mvk::SPIRVToMSLConversionConfiguration& shaderConfig,
    const VkPipelineShaderStageCreateInfo* pMeshSS,
    const VkPipelineShaderStageCreateInfo* pFragmentSS, uint32_t viewMask) {
	const auto& capabilities = getDevice()->shaderBackend().capabilities();
	bool discard = pCreateInfo->pRasterizationState->rasterizerDiscardEnable;
	const char* rejectionReason = capabilities.graphicsRejectionReason(false, true, discard, mvkIsMultiview(viewMask));
	if (!rejectionReason && pCreateInfo->pMultisampleState &&
	    !capabilities.supportsSampleShading(pCreateInfo->pMultisampleState->sampleShadingEnable,
	                                        pCreateInfo->pMultisampleState->minSampleShading)) {
		rejectionReason = "sample_shading";
	}
	if (rejectionReason || !_isRasterizing) {
		mvkMetalIRRecordPipelineRejection(getDevice(), rejectionReason ? rejectionReason : "mesh_rasterization_disabled");
		setConfigurationResult(reportError(VK_ERROR_FEATURE_NOT_PRESENT,
			"MetalIR mesh pipeline rejected: %s, mesh_code=%016zx, fragment_code=%016zx; MSL fallback disabled.",
			rejectionReason ? rejectionReason : "mesh_rasterization_disabled", _vertexModule->getKey().codeHash,
			_fragmentModule ? _fragmentModule->getKey().codeHash : 0));
		return false;
	}
	// The mesh function writes clip-space positions, so it takes the vertex stage's transforms.
	uint32_t transforms = (shaderConfig.options.shouldFlipVertexY ? MVK_METAL_IR_FLIP_Y : 0) |
		(shaderConfig.options.shouldFixupClipSpace ? MVK_METAL_IR_CLIP_HALF_Z : 0);
	auto meshIR = getDevice()->shaderBackend().compileStage({this, _layout, _vertexModule, pMeshSS, transforms, 0, {}}).artifactOwner;
	if (!meshIR) return false;
	auto fragmentIR = pFragmentSS ? getDevice()->shaderBackend().compileStage({this, _layout, _fragmentModule, pFragmentSS, 0, 0, {},
		!shaderConfig.options.mslOptions.enable_frag_depth_builtin,
		!shaderConfig.options.mslOptions.enable_frag_stencil_ref_builtin}).artifactOwner : nullptr;
	if (pFragmentSS && !fragmentIR) return false;

	_stageResources[kMVKShaderStageVertex].metalIR = meshIR->metadata;
	_stageResources[kMVKShaderStageFragment].metalIR = fragmentIR ? fragmentIR->metadata : nullptr;
	mvkPopulateMetalIRResidencyOperations(_layout, _stageResources[kMVKShaderStageVertex]);
	if (fragmentIR) mvkPopulateMetalIRResidencyOperations(_layout, _stageResources[kMVKShaderStageFragment]);
	_meshThreadgroupSize = MTLSizeMake(meshIR->threadgroupSize[0], meshIR->threadgroupSize[1], meshIR->threadgroupSize[2]);

	MTLRenderPipelineDescriptor* rastDesc = [MTLRenderPipelineDescriptor new];		// temp retain
	addFragmentOutputToPipeline(rastDesc, pCreateInfo);
	plDesc.meshFunction = meshIR->function;
	plDesc.fragmentFunction = fragmentIR ? fragmentIR->function : nil;
	plDesc.rasterizationEnabled = true;
	mvkCopyMeshRenderTargets(plDesc, rastDesc);
	[rastDesc release];																	// temp release
	return true;
}
