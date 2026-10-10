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
	bool tryMetalIR = !isTessellationPipeline() && !isMeshPipeline() &&
		!pCreateInfo->pRasterizationState->rasterizerDiscardEnable &&
		!mvkIsMultiview(viewMask);
	if (tryMetalIR && pCreateInfo->pVertexInputState) {
		for (const auto& binding : MVKArrayRef(pCreateInfo->pVertexInputState->pVertexBindingDescriptions,
		                                     pCreateInfo->pVertexInputState->vertexBindingDescriptionCount)) {
			if (getMetalBufferIndexForVertexAttributeBinding(binding.binding) <= 2) {
				tryMetalIR = false;
				conflictingVertexBinding = binding.binding;
			}
		}
	}
	if (tryMetalIR && pCreateInfo->pMultisampleState && pCreateInfo->pMultisampleState->sampleShadingEnable &&
		pCreateInfo->pMultisampleState->minSampleShading != 0.0f) tryMetalIR = false;
	if (!tryMetalIR) {
        setConfigurationResult(reportError(VK_ERROR_FEATURE_NOT_PRESENT,
            "MetalIR graphics pipeline rejected: topology=%u, polygon=%u, discard=%u, view_mask=0x%x, sample_shading=%u, min_sample_shading=%.3f, vertex_binding=%u, vertex_code=%016zx, fragment_code=%016zx; MSL fallback disabled.",
            pCreateInfo->pInputAssemblyState ? pCreateInfo->pInputAssemblyState->topology : UINT32_MAX,
            pCreateInfo->pRasterizationState->polygonMode,
            pCreateInfo->pRasterizationState->rasterizerDiscardEnable,
            viewMask,
            pCreateInfo->pMultisampleState ? pCreateInfo->pMultisampleState->sampleShadingEnable : 0,
            pCreateInfo->pMultisampleState ? pCreateInfo->pMultisampleState->minSampleShading : 0.0f,
            conflictingVertexBinding, _vertexModule->getKey().codeHash,
            _fragmentModule ? _fragmentModule->getKey().codeHash : 0));
        return false;
    }
	if (tryMetalIR) {
		uint32_t transforms = (shaderConfig.options.shouldFlipVertexY ? MVK_METAL_IR_FLIP_Y : 0) |
			(shaderConfig.options.shouldFixupClipSpace ? MVK_METAL_IR_CLIP_HALF_Z : 0);
        uint32_t rasterOptions = getPrimitiveTopologyClass() == MTLPrimitiveTopologyClassPoint
            ? MVK_METAL_IR_RENDERING_POINTS : 0;
        auto vertexIR = mvkCompileMetalIR(this, _layout, _vertexModule, pVertexSS, transforms, rasterOptions);
		if (vertexIR && getPrimitiveTopologyClass() == MTLPrimitiveTopologyClassPoint &&
            !(vertexIR->runtimeFlags & (MVK_METAL_IR_UNIT_POINT_SIZE | MVK_METAL_IR_NATIVE_POINT_SIZE))) {
			setConfigurationResult(reportError(VK_ERROR_FEATURE_NOT_PRESENT,
                "MetalIR point pipeline has no native point size or proven one-pixel default: vertex_code=%016zx, fragment_code=%016zx; MSL fallback disabled.",
				_vertexModule->getKey().codeHash, _fragmentModule ? _fragmentModule->getKey().codeHash : 0));
			return false;
		}
		if (vertexIR && (vertexIR->runtimeFlags & MVK_METAL_IR_DRAW_PARAMETERS) && pCreateInfo->pVertexInputState) {
			for (const auto& binding : MVKArrayRef(pCreateInfo->pVertexInputState->pVertexBindingDescriptions,
				pCreateInfo->pVertexInputState->vertexBindingDescriptionCount)) {
				uint32_t index = getMetalBufferIndexForVertexAttributeBinding(binding.binding);
				if (index == 4 || index == 5) {
					setConfigurationResult(reportError(VK_ERROR_FEATURE_NOT_PRESENT, "MetalIR runtime draw buffer conflicts with vertex binding; MSL fallback disabled."));
					vertexIR.reset(); break;
				}
			}
		}
        auto fragmentIR = vertexIR && pFragmentSS ? mvkCompileMetalIR(this, _layout, _fragmentModule, pFragmentSS, 0, rasterOptions) : nullptr;
        if (fragmentIR && getPrimitiveTopologyClass() == MTLPrimitiveTopologyClassPoint && fragmentIR->usesPointCoordinates &&
            !(fragmentIR->runtimeFlags & MVK_METAL_IR_NATIVE_POINT_COORDINATES)) {
			setConfigurationResult(reportError(VK_ERROR_FEATURE_NOT_PRESENT,
                "MetalIR point coordinates have no native rasterizer input; MSL fallback disabled."));
			return false;
		}
		if (vertexIR && (!pFragmentSS || fragmentIR)) {
			_stageResources[kMVKShaderStageVertex].metalIR = vertexIR->metadata;
			_stageResources[kMVKShaderStageFragment].metalIR = fragmentIR ? fragmentIR->metadata : nullptr;
			plDesc.vertexFunction = vertexIR->function;
			plDesc.fragmentFunction = fragmentIR ? fragmentIR->function : nil;
			_isRasterizing = true;
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
