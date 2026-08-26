/*
 * MVKCmdRendering.mm
 *
 * Copyright (c) 2015-2026 The Brenwill Workshop Ltd. (http://www.brenwill.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MVKCmdRendering.h"
#include "MVKCommandBuffer.h"
#include "MVKCommandPool.h"
#include "MVKFramebuffer.h"
#include "MVKRenderPass.h"
#include "MVKPipeline.h"
#include "MVKImage.h"
#include "MVKFoundation.h"
#include "mvk_datatypes.hpp"


#pragma mark -
#pragma mark MVKCmdBeginRenderPassBase

VkResult MVKCmdBeginRenderPassBase::setContent(MVKCommandBuffer* cmdBuff,
											   const VkRenderPassBeginInfo* pRenderPassBegin,
											   const VkSubpassBeginInfo* pSubpassBeginInfo) {
	_contents = pSubpassBeginInfo->contents;
	_renderPass = (MVKRenderPass*)pRenderPassBegin->renderPass;
	_framebuffer = (MVKFramebuffer*)pRenderPassBegin->framebuffer;
	_renderArea = pRenderPassBegin->renderArea;

	cmdBuff->_currentSubpassInfo.beginRenderpass(_renderPass);

	return VK_SUCCESS;
}


#pragma mark -
#pragma mark MVKCmdBeginRenderPass

template <size_t N_CV, size_t N_A>
VkResult MVKCmdBeginRenderPass<N_CV, N_A>::setContent(MVKCommandBuffer* cmdBuff,
													  const VkRenderPassBeginInfo* pRenderPassBegin,
													  const VkSubpassBeginInfo* pSubpassBeginInfo,
													  MVKArrayRef<MVKImageView*> attachments) {
	MVKCmdBeginRenderPassBase::setContent(cmdBuff, pRenderPassBegin, pSubpassBeginInfo);

	_attachments.assign(attachments.begin(), attachments.end());
	_clearValues.assign(pRenderPassBegin->pClearValues,
						pRenderPassBegin->pClearValues + pRenderPassBegin->clearValueCount);

	MVKRenderSubpass* subpass = _renderPass && _renderPass->getSubpassCount() == 1
		? _renderPass->getSubpass(0)
		: nullptr;
	VkExtent2D framebufferExtent = _framebuffer
		? _framebuffer->getExtent2D()
		: VkExtent2D{};
	_supportsMetal4Encoding = subpass &&
		_contents == VK_SUBPASS_CONTENTS_INLINE &&
		_framebuffer->getLayerCount() == 1 &&
		_renderArea.offset.x == 0 && _renderArea.offset.y == 0 &&
		_renderArea.extent.width == framebufferExtent.width &&
		_renderArea.extent.height == framebufferExtent.height &&
		!subpass->isMultiview() &&
		subpass->getSampleCount() == VK_SAMPLE_COUNT_1_BIT &&
		subpass->getColorAttachmentCount() > 0 &&
		subpass->getColorAttachmentCount() <= kMVKMaxColorAttachmentCount &&
		!subpass->isStencilAttachmentUsed();
	if (_supportsMetal4Encoding) {
		bool hasUsedColorAttachment = false;
		for (uint32_t colorIndex = 0;
			 colorIndex < subpass->getColorAttachmentCount();
			 colorIndex++) {
			hasUsedColorAttachment |= subpass->isColorAttachmentUsed(colorIndex);
		}
		_supportsMetal4Encoding = hasUsedColorAttachment;
	}
	if (_supportsMetal4Encoding) {
		for (MVKImageView* attachment : _attachments) {
			id<MTLTexture> texture = attachment ? attachment->getMTLTexture() : nil;
			if (!attachment || !texture ||
				attachment->getPlaneCount() != 1 ||
				attachment->getSampleCount() != VK_SAMPLE_COUNT_1_BIT ||
				attachment->getMTLTextureType() != MTLTextureType2D ||
				attachment->getPackedSwizzle() != 0 ||
				texture.width != framebufferExtent.width ||
				texture.height != framebufferExtent.height) {
				_supportsMetal4Encoding = false;
				break;
			}
		}
	}

	return VK_SUCCESS;
}

template <size_t N_CV, size_t N_A>
void MVKCmdBeginRenderPass<N_CV, N_A>::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->beginRenderpass(this,
								_contents,
								_renderPass,
								_framebuffer,
								_renderArea,
								_clearValues.contents(),
								_attachments.contents(),
								kMVKCommandUseBeginRenderPass);
}

template <size_t N_CV, size_t N_A>
bool MVKCmdBeginRenderPass<N_CV, N_A>::prepareMetal4Encoding(
	MVKMetal4CommandEncoder* cmdEncoder) {
	if (!cmdEncoder || !_supportsMetal4Encoding) { return false; }
	for (MVKImageView* attachment : _attachments) {
		if (!cmdEncoder->useImageView(attachment)) { return false; }
	}
	return true;
}

template <size_t N_CV, size_t N_A>
bool MVKCmdBeginRenderPass<N_CV, N_A>::encodeMetal4(
	MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && _supportsMetal4Encoding &&
		cmdEncoder->beginRenderPass(_renderPass,
									_framebuffer,
									_renderArea,
									_clearValues.data(),
									_clearValues.size(),
									_attachments.data(),
									_attachments.size());
}

template class MVKCmdBeginRenderPass<1, 0>;
template class MVKCmdBeginRenderPass<2, 0>;
template class MVKCmdBeginRenderPass<9, 0>;

template class MVKCmdBeginRenderPass<1, 1>;
template class MVKCmdBeginRenderPass<2, 1>;
template class MVKCmdBeginRenderPass<9, 1>;

template class MVKCmdBeginRenderPass<1, 2>;
template class MVKCmdBeginRenderPass<2, 2>;
template class MVKCmdBeginRenderPass<9, 2>;

template class MVKCmdBeginRenderPass<1, 9>;
template class MVKCmdBeginRenderPass<2, 9>;
template class MVKCmdBeginRenderPass<9, 9>;

#pragma mark -
#pragma mark MVKCmdNextSubpass

VkResult MVKCmdNextSubpass::setContent(MVKCommandBuffer* cmdBuff,
									   VkSubpassContents contents) {
	_contents = contents;

	cmdBuff->_currentSubpassInfo.nextSubpass();

	return VK_SUCCESS;
}

VkResult MVKCmdNextSubpass::setContent(MVKCommandBuffer* cmdBuff,
									   const VkSubpassBeginInfo* pBeginSubpassInfo,
									   const VkSubpassEndInfo* pEndSubpassInfo) {
	return setContent(cmdBuff, pBeginSubpassInfo->contents);
}

void MVKCmdNextSubpass::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->beginNextSubpass(this, _contents);
}


#pragma mark -
#pragma mark MVKCmdEndRenderPass

VkResult MVKCmdEndRenderPass::setContent(MVKCommandBuffer* cmdBuff) {
	cmdBuff->_currentSubpassInfo = {};
	return VK_SUCCESS;
}

VkResult MVKCmdEndRenderPass::setContent(MVKCommandBuffer* cmdBuff,
										 const VkSubpassEndInfo* pEndSubpassInfo) {
	return setContent(cmdBuff);
}

void MVKCmdEndRenderPass::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->endRenderpass();
}

bool MVKCmdEndRenderPass::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && cmdEncoder->endRendering();
}


#pragma mark -
#pragma mark MVKCmdBeginRendering

static bool mvkSupportsMetal4RenderingAttachment(const VkRenderingAttachmentInfo& attachment,
											 const VkRect2D& renderArea,
											 bool depthAttachment) {
	bool supportedLayout = depthAttachment
		? (attachment.imageLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
		   attachment.imageLayout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
		   attachment.imageLayout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL ||
		   attachment.imageLayout == VK_IMAGE_LAYOUT_GENERAL)
		: (attachment.imageLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
		   attachment.imageLayout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL ||
		   attachment.imageLayout == VK_IMAGE_LAYOUT_GENERAL);
	if (attachment.pNext ||
		!attachment.imageView ||
		attachment.resolveMode != VK_RESOLVE_MODE_NONE ||
		attachment.resolveImageView ||
		(attachment.loadOp != VK_ATTACHMENT_LOAD_OP_LOAD &&
		 attachment.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR &&
		 attachment.loadOp != VK_ATTACHMENT_LOAD_OP_DONT_CARE) ||
		(attachment.storeOp != VK_ATTACHMENT_STORE_OP_STORE &&
		 attachment.storeOp != VK_ATTACHMENT_STORE_OP_DONT_CARE) ||
		!supportedLayout) {
		return false;
	}

	auto* imageView = (MVKImageView*)attachment.imageView;
	id<MTLTexture> texture = imageView ? imageView->getMTLTexture() : nil;
	if (!imageView || !texture ||
		imageView->getPlaneCount() != 1 ||
		imageView->getSampleCount() != VK_SAMPLE_COUNT_1_BIT ||
		imageView->getMTLTextureType() != MTLTextureType2D ||
		imageView->getPackedSwizzle() != 0) {
		return false;
	}

	return renderArea.extent.width == texture.width &&
		renderArea.extent.height == texture.height;
}

static bool mvkSupportsMetal4RenderingInfo(const VkRenderingInfo& renderingInfo) {
	if (renderingInfo.pNext ||
		renderingInfo.flags != 0 ||
		renderingInfo.viewMask != 0 ||
		renderingInfo.layerCount != 1 ||
		renderingInfo.colorAttachmentCount == 0 ||
		renderingInfo.colorAttachmentCount > kMVKMaxColorAttachmentCount ||
		!renderingInfo.pColorAttachments ||
		renderingInfo.renderArea.offset.x != 0 ||
		renderingInfo.renderArea.offset.y != 0) {
		return false;
	}

	if (renderingInfo.pStencilAttachment) {
		return false;
	}

	for (uint32_t colorIndex = 0;
		 colorIndex < renderingInfo.colorAttachmentCount;
		 colorIndex++) {
		if (!mvkSupportsMetal4RenderingAttachment(
				renderingInfo.pColorAttachments[colorIndex],
				renderingInfo.renderArea,
				false)) {
			return false;
		}
	}

	return !renderingInfo.pDepthAttachment ||
		mvkSupportsMetal4RenderingAttachment(
			*renderingInfo.pDepthAttachment, renderingInfo.renderArea, true);
}

template <size_t N>
VkResult MVKCmdBeginRendering<N>::setContent(MVKCommandBuffer* cmdBuff,
											 const VkRenderingInfo* pRenderingInfo) {
	_supportsMetal4Encoding = mvkSupportsMetal4RenderingInfo(*pRenderingInfo);
	_renderingInfo = *pRenderingInfo;

	// Copy attachments content, redirect info pointers to copied content, and remove any stale pNext refs
	_colorAttachments.assign(_renderingInfo.pColorAttachments,
							 _renderingInfo.pColorAttachments + _renderingInfo.colorAttachmentCount);
	_renderingInfo.pColorAttachments = _colorAttachments.data();
	for (auto& caAtt : _colorAttachments) { caAtt.pNext = nullptr; }

	if (mvkSetOrClear(&_depthAttachment, _renderingInfo.pDepthAttachment)) {
		_renderingInfo.pDepthAttachment = &_depthAttachment;
	}
	if (mvkSetOrClear(&_stencilAttachment, _renderingInfo.pStencilAttachment)) {
		_renderingInfo.pStencilAttachment = &_stencilAttachment;
	}

	cmdBuff->_currentSubpassInfo.beginRendering(pRenderingInfo->viewMask);

	return VK_SUCCESS;
}

template <size_t N>
void MVKCmdBeginRendering<N>::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->beginRendering(this, &_renderingInfo);
}

template <size_t N>
bool MVKCmdBeginRendering<N>::prepareMetal4Encoding(MVKMetal4CommandEncoder* cmdEncoder) {
	if (!cmdEncoder || !_supportsMetal4Encoding) { return false; }
	for (uint32_t colorIndex = 0;
		 colorIndex < _renderingInfo.colorAttachmentCount;
		 colorIndex++) {
		auto* imageView =
			(MVKImageView*)_renderingInfo.pColorAttachments[colorIndex].imageView;
		if (!cmdEncoder->useImageView(imageView)) { return false; }
	}
	if (_renderingInfo.pDepthAttachment &&
		!cmdEncoder->useImageView(
			(MVKImageView*)_renderingInfo.pDepthAttachment->imageView)) {
		return false;
	}
	return true;
}

template <size_t N>
bool MVKCmdBeginRendering<N>::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && _supportsMetal4Encoding && cmdEncoder->beginRendering(_renderingInfo);
}

template class MVKCmdBeginRendering<1>;
template class MVKCmdBeginRendering<2>;
template class MVKCmdBeginRendering<4>;
template class MVKCmdBeginRendering<8>;


#pragma mark -
#pragma mark MVKCmdSetRenderingAttachmentLocations

// Resize dst to count, then if pSrc is not null, populate dst from it,
// otherwise fill dst with ascending values starting at zero.
template<typename Vec>
void mvkPopulateFromOrFillAscending(Vec& dst, const uint32_t* pSrc, size_t count) {
	dst.resize(count);
	for (uint32_t i = 0; i < count; i++) { dst[i] = pSrc ? pSrc[i] : i; }
}

VkResult MVKCmdSetRenderingAttachmentLocations::setContent(MVKCommandBuffer* cmdBuff,
														   const VkRenderingAttachmentLocationInfo* pLocationInfo) {
	mvkPopulateFromOrFillAscending(_colorAttachmentLocations,
								   pLocationInfo->pColorAttachmentLocations,
								   pLocationInfo->colorAttachmentCount);
	return VK_SUCCESS;
}

void MVKCmdSetRenderingAttachmentLocations::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->updateColorAttachmentLocations(_colorAttachmentLocations.contents());
}


#pragma mark -
#pragma mark MVKCmdSetRenderingInputAttachmentIndices

VkResult MVKCmdSetRenderingInputAttachmentIndices::setContent(MVKCommandBuffer* cmdBuff,
															  const VkRenderingInputAttachmentIndexInfo* pInputAttachmentIndexInfo) {
	mvkPopulateFromOrFillAscending(_colorAttachmentInputIndices,
								   pInputAttachmentIndexInfo->pColorAttachmentInputIndices,
								   pInputAttachmentIndexInfo->colorAttachmentCount);

	_hasDepthInputAttachmentIndex = pInputAttachmentIndexInfo->pDepthInputAttachmentIndex;
	_depthInputAttachmentIndex = _hasDepthInputAttachmentIndex ? *pInputAttachmentIndexInfo->pDepthInputAttachmentIndex : 0;

	_hasStencilInputAttachmentIndex = pInputAttachmentIndexInfo->pStencilInputAttachmentIndex;
	_stencilInputAttachmentIndex = _hasStencilInputAttachmentIndex ? *pInputAttachmentIndexInfo->pStencilInputAttachmentIndex : 0;

	return VK_SUCCESS;
}

void MVKCmdSetRenderingInputAttachmentIndices::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->updateAttachmentInputIndices(_colorAttachmentInputIndices.contents(),
											 _hasDepthInputAttachmentIndex ? &_depthInputAttachmentIndex : nullptr,
											 _hasStencilInputAttachmentIndex ? &_stencilInputAttachmentIndex : nullptr);
}


#pragma mark -
#pragma mark MVKCmdEndRendering

VkResult MVKCmdEndRendering::setContent(MVKCommandBuffer* cmdBuff) {
	cmdBuff->_currentSubpassInfo = {};
	return VK_SUCCESS;
}

void MVKCmdEndRendering::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->endRendering();
}

bool MVKCmdEndRendering::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && cmdEncoder->endRendering();
}


#pragma mark -
#pragma mark MVKCmdSetSampleLocations

VkResult MVKCmdSetSampleLocations::setContent(MVKCommandBuffer* cmdBuff,
											  const VkSampleLocationsInfoEXT* pSampleLocationsInfo) {
	_sampleLocations.clear();
	for (uint32_t slIdx = 0; slIdx < pSampleLocationsInfo->sampleLocationsCount; slIdx++) {
		_sampleLocations.push_back(pSampleLocationsInfo->pSampleLocations[slIdx]);
	}
	return VK_SUCCESS;
}

void MVKCmdSetSampleLocations::encode(MVKCommandEncoder* cmdEncoder) {
	size_t count = std::min<size_t>(_sampleLocations.size(), kMVKMaxSampleCount);
	MVKVulkanGraphicsCommandEncoderState& state = cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::SampleLocations);
	state._renderState.numSampleLocations = static_cast<uint8_t>(count);
	MTLSamplePosition* write = state._sampleLocations;
	for (size_t i = 0; i < count; i++) {
		write[i] = MTLSamplePositionMake(
			mvkClamp(_sampleLocations[i].x, kMVKMinSampleLocationCoordinate, kMVKMaxSampleLocationCoordinate),
			mvkClamp(_sampleLocations[i].y, kMVKMinSampleLocationCoordinate, kMVKMaxSampleLocationCoordinate));
	}
}


#pragma mark -
#pragma mark MVKCmdSetSampleLocationsEnable

void MVKCmdSetSampleLocationsEnable::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::SampleLocationsEnable)._renderState.enable.set(MVKRenderStateEnableFlag::SampleLocations, _value);
}


#pragma mark -
#pragma mark MVKCmdSetViewport

template <size_t N>
VkResult MVKCmdSetViewport<N>::setContent(MVKCommandBuffer* cmdBuff,
										  uint32_t firstViewport,
										  uint32_t viewportCount,
										  const VkViewport* pViewports) {
	_firstViewport = firstViewport;
	_viewports.clear();
	_viewports.reserve(viewportCount);
	for (uint32_t vpIdx = 0; vpIdx < viewportCount; vpIdx++) {
		_viewports.push_back(pViewports[vpIdx]);
	}

	return VK_SUCCESS;
}

template <size_t N>
void MVKCmdSetViewport<N>::encode(MVKCommandEncoder* cmdEncoder) {
	uint32_t end = std::min(_firstViewport + static_cast<uint32_t>(_viewports.size()), kMVKMaxViewportScissorCount);
	MVKVulkanGraphicsCommandEncoderState& state = cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::Viewports);
	state._renderState.numViewports = std::max(static_cast<uint8_t>(end), cmdEncoder->getVkGraphics()._renderState.numViewports);
	for (uint32_t i = _firstViewport; i < end; i++)
		state._viewports[i] = _viewports[i - _firstViewport];
}

template <size_t N>
bool MVKCmdSetViewport<N>::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && supportsMetal4Encoding() &&
		cmdEncoder->setViewports(
			_firstViewport, static_cast<uint32_t>(_viewports.size()), _viewports.data());
}

template class MVKCmdSetViewport<1>;
template class MVKCmdSetViewport<kMVKMaxViewportScissorCount>;


#pragma mark -
#pragma mark MVKCmdSetScissor

template <size_t N>
VkResult MVKCmdSetScissor<N>::setContent(MVKCommandBuffer* cmdBuff,
										 uint32_t firstScissor,
										 uint32_t scissorCount,
										 const VkRect2D* pScissors) {
	_firstScissor = firstScissor;
	_scissors.clear();
	_scissors.reserve(scissorCount);
	for (uint32_t sIdx = 0; sIdx < scissorCount; sIdx++) {
		_scissors.push_back(pScissors[sIdx]);
	}

	return VK_SUCCESS;
}

template <size_t N>
void MVKCmdSetScissor<N>::encode(MVKCommandEncoder* cmdEncoder) {
	uint32_t end = std::min(_firstScissor + static_cast<uint32_t>(_scissors.size()), kMVKMaxViewportScissorCount);
	MVKVulkanGraphicsCommandEncoderState& state = cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::Scissors);
	state._renderState.numScissors = std::max(static_cast<uint8_t>(end), cmdEncoder->getVkGraphics()._renderState.numScissors);
	for (uint32_t i = _firstScissor; i < end; i++)
		state._scissors[i] = _scissors[i - _firstScissor];
}

template <size_t N>
bool MVKCmdSetScissor<N>::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && supportsMetal4Encoding() &&
		cmdEncoder->setScissors(
			_firstScissor, static_cast<uint32_t>(_scissors.size()), _scissors.data());
}

template class MVKCmdSetScissor<1>;
template class MVKCmdSetScissor<kMVKMaxViewportScissorCount>;


#pragma mark -
#pragma mark MVKCmdSetDepthBias

void MVKCmdSetDepthBias::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::DepthBias)._renderState.depthBias = _value;
}

bool MVKCmdSetDepthBias::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	// Metal 4 pipelines reach this command only when depth bias is statically
	// disabled, so the Vulkan value is semantically inert. Pipelines that can
	// enable depth bias remain ineligible and force whole-buffer fallback.
	return cmdEncoder;
}


#pragma mark -
#pragma mark MVKCmdSetDepthBiasEnable

void MVKCmdSetDepthBiasEnable::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::DepthBiasEnable)._renderState.enable.set(MVKRenderStateEnableFlag::DepthBias, _value);
}


#pragma mark -
#pragma mark MVKCmdSetBlendConstants

void MVKCmdSetBlendConstants::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::BlendConstants)._renderState.blendConstants = _value;
}

bool MVKCmdSetBlendConstants::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && cmdEncoder->setBlendConstants(_value.float32);
}


#pragma mark -
#pragma mark MVKCmdSetDepthTestEnable

void MVKCmdSetDepthTestEnable::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::DepthTestEnable)._renderState.enable.set(MVKRenderStateEnableFlag::DepthTest, _value);
}


#pragma mark -
#pragma mark MVKCmdSetDepthWriteEnable

void MVKCmdSetDepthWriteEnable::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::DepthWriteEnable)._renderState.depthStencil.depthWriteEnabled = _value;
}


#pragma mark -
#pragma mark MVKCmdSetDepthClipEnable

void MVKCmdSetDepthClipEnable::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::DepthClipEnable)._renderState.enable.set(MVKRenderStateEnableFlag::DepthClamp, !_value);
}


#pragma mark -
#pragma mark MVKCmdSetDepthCompareOp

void MVKCmdSetDepthCompareOp::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::DepthCompareOp)._renderState.depthStencil.depthCompareFunction = _value;
}


#pragma mark -
#pragma mark MVKCmdSetDepthBounds

void MVKCmdSetDepthBounds::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::DepthBounds)._renderState.depthBounds = _value;
}


#pragma mark -
#pragma mark MVKCmdSetDepthBoundsTestEnable

void MVKCmdSetDepthBoundsTestEnable::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::DepthBoundsTestEnable)._renderState.enable.set(MVKRenderStateEnableFlag::DepthBoundsTest, _value);
}


#pragma mark -
#pragma mark MVKCmdSetStencilTestEnable

void MVKCmdSetStencilTestEnable::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::StencilTestEnable)._renderState.depthStencil.stencilTestEnabled = _value;
}


#pragma mark -
#pragma mark MVKCmdSetStencilOp

VkResult MVKCmdSetStencilOp::setContent(MVKCommandBuffer* cmdBuff,
										VkStencilFaceFlags faceMask,
										VkStencilOp failOp,
										VkStencilOp passOp,
										VkStencilOp depthFailOp,
										VkCompareOp compareOp) {
	_faceMask = faceMask;
	_failOp = failOp;
	_passOp = passOp;
	_depthFailOp = depthFailOp;
	_compareOp = compareOp;
	return VK_SUCCESS;
}

void MVKCmdSetStencilOp::encode(MVKCommandEncoder* cmdEncoder) {
	MVKMTLStencilOps op;
	op.stencilCompareFunction = mvkMTLCompareFunctionFromVkCompareOp(_compareOp);
	op.stencilFailureOperation = mvkMTLStencilOperationFromVkStencilOp(_failOp);
	op.depthFailureOperation = mvkMTLStencilOperationFromVkStencilOp(_depthFailOp);
	op.depthStencilPassOperation = mvkMTLStencilOperationFromVkStencilOp(_passOp);
	MVKVulkanGraphicsCommandEncoderState& state = cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::StencilOp);
	if (_faceMask & VK_STENCIL_FACE_FRONT_BIT)
		state._renderState.depthStencil.frontFaceStencilData.op = op;
	if (_faceMask & VK_STENCIL_FACE_BACK_BIT)
		state._renderState.depthStencil.backFaceStencilData.op = op;
}


#pragma mark -
#pragma mark MVKCmdSetStencilCompareMask

VkResult MVKCmdSetStencilCompareMask::setContent(MVKCommandBuffer* cmdBuff,
												 VkStencilFaceFlags faceMask,
												 uint32_t stencilCompareMask) {
    _faceMask = faceMask;
    _stencilCompareMask = stencilCompareMask;

	return VK_SUCCESS;
}

void MVKCmdSetStencilCompareMask::encode(MVKCommandEncoder* cmdEncoder) {
	MVKVulkanGraphicsCommandEncoderState& state = cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::StencilCompareMask);
	if (_faceMask & VK_STENCIL_FACE_FRONT_BIT)
		state._renderState.depthStencil.frontFaceStencilData.readMask = _stencilCompareMask;
	if (_faceMask & VK_STENCIL_FACE_BACK_BIT)
		state._renderState.depthStencil.backFaceStencilData.readMask = _stencilCompareMask;
}

bool MVKCmdSetStencilCompareMask::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && cmdEncoder->setStencilCompareMask(_faceMask, _stencilCompareMask);
}


#pragma mark -
#pragma mark MVKCmdSetStencilWriteMask

VkResult MVKCmdSetStencilWriteMask::setContent(MVKCommandBuffer* cmdBuff,
											   VkStencilFaceFlags faceMask,
											   uint32_t stencilWriteMask) {
    _faceMask = faceMask;
    _stencilWriteMask = stencilWriteMask;

	return VK_SUCCESS;
}

void MVKCmdSetStencilWriteMask::encode(MVKCommandEncoder* cmdEncoder) {
	MVKVulkanGraphicsCommandEncoderState& state = cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::StencilWriteMask);
	if (_faceMask & VK_STENCIL_FACE_FRONT_BIT)
		state._renderState.depthStencil.frontFaceStencilData.writeMask = _stencilWriteMask;
	if (_faceMask & VK_STENCIL_FACE_BACK_BIT)
		state._renderState.depthStencil.backFaceStencilData.writeMask = _stencilWriteMask;
}

bool MVKCmdSetStencilWriteMask::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && cmdEncoder->setStencilWriteMask(_faceMask, _stencilWriteMask);
}


#pragma mark -
#pragma mark MVKCmdSetStencilReference

VkResult MVKCmdSetStencilReference::setContent(MVKCommandBuffer* cmdBuff,
											   VkStencilFaceFlags faceMask,
											   uint32_t stencilReference) {
    _faceMask = faceMask;
    _stencilReference = stencilReference;

	return VK_SUCCESS;
}

void MVKCmdSetStencilReference::encode(MVKCommandEncoder* cmdEncoder) {
	MVKVulkanGraphicsCommandEncoderState& state = cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::StencilReference);
	if (_faceMask & VK_STENCIL_FACE_FRONT_BIT)
		state._renderState.stencilReference.frontFaceValue = _stencilReference;
	if (_faceMask & VK_STENCIL_FACE_BACK_BIT)
		state._renderState.stencilReference.backFaceValue = _stencilReference;
}

bool MVKCmdSetStencilReference::encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) {
	return cmdEncoder && cmdEncoder->setStencilReference(_faceMask, _stencilReference);
}


#pragma mark -
#pragma mark MVKCmdSetCullMode

void MVKCmdSetCullMode::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::CullMode)._renderState.setCullMode(_value);
}


#pragma mark -
#pragma mark MVKCmdSetFrontFace

void MVKCmdSetFrontFace::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::FrontFace)._renderState.setFrontFace(_value);
}


#pragma mark -
#pragma mark MVKCmdSetPatchControlPoints

void MVKCmdSetPatchControlPoints::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::PatchControlPoints)._renderState.patchControlPoints = static_cast<uint8_t>(_value);
}


#pragma mark -
#pragma mark MVKCmdSetPolygonMode

void MVKCmdSetPolygonMode::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::PolygonMode)._renderState.setPolygonMode(_value);
}


#pragma mark -
#pragma mark MVKCmdSetLineRasterizationMode

void MVKCmdSetLineRasterizationMode::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::LineRasterizationMode)._renderState.setLineRasterizationMode(_value);
}


#pragma mark -
#pragma mark MVKCmdSetLineWidth

void MVKCmdSetLineWidth::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::LineWidth)._renderState.lineWidth = _value;
}


#pragma mark -
#pragma mark MVKCmdSetPrimitiveTopology

void MVKCmdSetPrimitiveTopology::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::PrimitiveTopology)._renderState.primitiveType = mvkMTLPrimitiveTypeFromVkPrimitiveTopology(_value);
}


#pragma mark -
#pragma mark MVKCmdSetPrimitiveRestartEnable

void MVKCmdSetPrimitiveRestartEnable::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::PrimitiveRestartEnable)._renderState.enable.set(MVKRenderStateEnableFlag::PrimitiveRestart, _value);
}


#pragma mark -
#pragma mark MVKCmdSetRasterizerDiscardEnable

void MVKCmdSetRasterizerDiscardEnable::encode(MVKCommandEncoder* cmdEncoder) {
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::RasterizerDiscardEnable)._renderState.enable.set(MVKRenderStateEnableFlag::RasterizerDiscard, _value);
}


#pragma mark -
#pragma mark MVKCmdSetProvokingVertexMode

void MVKCmdSetProvokingVertexMode::encode(MVKCommandEncoder* cmdEncoder) {
#if MVK_USE_METAL_PRIVATE_API
	cmdEncoder->getState().updateDynamicState(MVKRenderStateFlag::ProvokingVertexMode)._renderState.provokingVertexMode = mvkMTLProvokingVertexModeFromVkProvokingVertexMode(_value);
#endif
}
