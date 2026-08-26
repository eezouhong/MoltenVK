/*
 * MVKCommand.h
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

#pragma once


#include "MVKObjectPool.h"

class MVKBuffer;
class MVKCommandBuffer;
class MVKCommandEncoder;
class MVKCommandPool;
class MVKComputePipeline;
struct MVKDescriptorSet;
class MVKGraphicsPipeline;
class MVKFramebuffer;
class MVKImage;
class MVKImageView;
class MVKPipelineLayout;
class MVKQueryPool;
class MVKRenderPass;
struct MVKPipelineBarrier;
struct MVKVertexMTLBufferBinding;


#pragma mark -
#pragma mark MVKMetal4CommandEncoder

/**
 * Backend-neutral command surface for the first usable Metal 4 execution
 * slice. Concrete Vulkan commands expose MoltenVK resources here instead of
 * Metal 4 protocol types, which keeps the command headers buildable with older
 * SDKs and keeps all Metal 4 lifetime/commit policy inside the queue backend.
 */
class MVKMetal4CommandEncoder {

public:
	virtual ~MVKMetal4CommandEncoder() = default;

	/** Registers a buffer and its underlying Metal allocation before execution is claimed. */
	virtual bool useBuffer(MVKBuffer* buffer) = 0;

	/** Registers an image and its underlying Metal texture before execution is claimed. */
	virtual bool useImage(MVKImage* image) = 0;

	/** Registers an image view used as a render attachment before execution is claimed. */
	virtual bool useImageView(MVKImageView* imageView) = 0;

	/** Registers query-pool storage before a reset is claimed. */
	virtual bool useQueryPool(MVKQueryPool* queryPool) = 0;

	/** Registers the single visibility-result buffer used by this strict slice. */
	virtual bool useVisibilityQueryPool(MVKQueryPool* queryPool) = 0;

	/** Creates resident staging storage for recorded vkCmdUpdateBuffer bytes. */
	virtual bool useUpdateBufferData(const void* data, size_t size) = 0;

	/** Registers a compute pipeline before execution is claimed. */
	virtual bool useComputePipeline(MVKComputePipeline* pipeline) = 0;

	/** Registers a graphics pipeline before execution is claimed. */
	virtual bool useGraphicsPipeline(MVKGraphicsPipeline* pipeline) = 0;

	/** Registers one immutable Metal 3 descriptor-set snapshot for MTL4 use. */
	virtual bool useDescriptorSet(MVKDescriptorSet* descriptorSet) = 0;

	virtual bool copyBuffer(MVKBuffer* srcBuffer,
							VkDeviceSize srcOffset,
							MVKBuffer* dstBuffer,
							VkDeviceSize dstOffset,
							VkDeviceSize size) = 0;

	virtual bool fillBuffer(MVKBuffer* dstBuffer,
							VkDeviceSize dstOffset,
							VkDeviceSize size,
							uint8_t value) = 0;

	virtual bool resetQueryPool(MVKQueryPool* queryPool,
							uint32_t firstQuery,
							uint32_t queryCount) = 0;

	virtual bool beginVisibilityQuery(MVKQueryPool* queryPool,
								 uint32_t query,
								 VkQueryControlFlags flags) = 0;
	virtual bool endVisibilityQuery(MVKQueryPool* queryPool, uint32_t query) = 0;

	virtual bool updateBuffer(MVKBuffer* dstBuffer,
						  VkDeviceSize dstOffset,
						  const void* data,
						  size_t size) = 0;

	virtual bool copyImage(MVKImage* srcImage,
						   uint8_t srcPlane,
						   const VkImageCopy2& region,
						   MVKImage* dstImage,
						   uint8_t dstPlane) = 0;

	virtual bool copyBufferImage(MVKBuffer* buffer,
							 MVKImage* image,
							 const VkBufferImageCopy2& region,
							 bool toImage) = 0;

	virtual bool bindComputePipeline(MVKComputePipeline* pipeline) = 0;

	virtual bool dispatchThreadgroups(uint32_t groupCountX,
								 uint32_t groupCountY,
								 uint32_t groupCountZ) = 0;

	virtual bool pipelineBarrier(VkPipelineStageFlags2 srcStages,
								 VkAccessFlags2 srcAccess,
								 VkPipelineStageFlags2 dstStages,
								 VkAccessFlags2 dstAccess) = 0;

	/** Defers image layout metadata until the Metal 4 command buffer commits. */
	virtual bool trackImageBarrier(const MVKPipelineBarrier& barrier) = 0;

	virtual bool beginRendering(const VkRenderingInfo& renderingInfo) = 0;
	virtual bool beginRenderPass(MVKRenderPass* renderPass,
								 MVKFramebuffer* framebuffer,
								 const VkRect2D& renderArea,
								 const VkClearValue* clearValues,
								 size_t clearValueCount,
								 MVKImageView*const* attachments,
								 size_t attachmentCount) = 0;
	virtual bool endRendering() = 0;
	virtual bool bindGraphicsPipeline(MVKGraphicsPipeline* pipeline) = 0;
	virtual bool setViewports(uint32_t firstViewport,
						 uint32_t viewportCount,
						 const VkViewport* viewports) = 0;
	virtual bool setScissors(uint32_t firstScissor,
						uint32_t scissorCount,
						const VkRect2D* scissors) = 0;
	virtual bool bindVertexBuffers(uint32_t firstBinding,
							   uint32_t bindingCount,
							   const MVKVertexMTLBufferBinding* bindings) = 0;
	virtual bool bindDescriptorSets(VkPipelineBindPoint bindPoint,
								MVKPipelineLayout* layout,
								uint32_t firstSet,
								uint32_t setCount,
								MVKDescriptorSet*const* descriptorSets) = 0;
	virtual bool draw(uint32_t firstVertex,
					  uint32_t vertexCount,
					  uint32_t firstInstance,
					  uint32_t instanceCount) = 0;
};


#pragma mark -
#pragma mark MVKCommandTypePool

/** A pool of MVKCommand instances of a particular type. */
template <class T>
class MVKCommandTypePool : public MVKObjectPool<T> {

public:

	/** Returns the Vulkan API opaque object controlling this object. */
	MVKVulkanAPIObject* getVulkanAPIObject() override { return nullptr; }

	MVKCommandTypePool(bool isPooling = true, const char* typeName = "MVKCommand") :
		MVKObjectPool<T>(isPooling), _typeName(typeName) {}

protected:
	T* newObject() override {
		T* command = new T();
		command->setMetal4CommandTypeName(_typeName);
		return command;
	}

	const char* _typeName;

};


#pragma mark -
#pragma mark MVKCommand

/**
 * Abstract class that represents a Vulkan command.
 *
 * To allow command contents to be populated in a standard way, all concrete
 * subclasses must support a public member function of the following form:
 *
 *     VkResult setContent(MVKCommandBuffer* cmdBuff, ...);
 */
class MVKCommand : public MVKBaseObject, public MVKLinkableMixin<MVKCommand> {

public:

	/** Returns the Vulkan API opaque object controlling this object. */
	MVKVulkanAPIObject* getVulkanAPIObject() override { return nullptr; }

	/** Encodes this command on the specified command encoder. */
	virtual void encode(MVKCommandEncoder* cmdEncoder) = 0;

	/** Returns the stable pooled command type used by bounded Metal 4 diagnostics. */
	const char* getMetal4CommandTypeName() const { return _metal4CommandTypeName; }

	/** Assigns the macro-generated pooled command type once at object construction. */
	void setMetal4CommandTypeName(const char* typeName) {
		_metal4CommandTypeName = typeName ? typeName : "MVKCommand";
	}

	/**
	 * Returns whether this command can be materialized by the current Metal 4
	 * backend. Commands are unsupported unless they explicitly opt in.
	 */
	virtual bool supportsMetal4Encoding() const { return false; }

	/**
	 * Returns a stable, bounded diagnostic reason when Metal 4 encoding is not
	 * supported. Subclasses may refine the pooled command type without allocating
	 * or logging per command.
	 */
	virtual const char* getMetal4UnsupportedReason() const {
		return getMetal4CommandTypeName();
	}

	/**
	 * Resolves and registers all Metal resources this command will touch. This is
	 * called before any Vulkan command-buffer execution is claimed, so failure can
	 * still select the legacy backend for the complete submission.
	 */
	virtual bool prepareMetal4Encoding(MVKMetal4CommandEncoder*) { return false; }

	/**
	 * Materializes this command into an uncommitted Metal 4 command buffer.
	 * The caller preflights the complete Vulkan submission before invoking it.
	 */
	virtual bool encodeMetal4(MVKMetal4CommandEncoder* cmdEncoder) { return false; }

protected:
	friend MVKCommandBuffer;
	const char* _metal4CommandTypeName = "MVKCommand";

	// Returns the command type pool used by this command, from the command pool.
	// This function is overridden in each concrete subclass declaration, but the implementation of
	// this function in each subclass is automatically generated in the MVKCommandPool implementation.
	virtual MVKCommandTypePool<MVKCommand>* getTypePool(MVKCommandPool* cmdPool) = 0;
};


#pragma mark -
#pragma mark MVKSingleValueCommand

/** Abstract class of a simple Vulkan command that simply holds a single value to encode. */
template <typename Tv>
class MVKSingleValueCommand : public MVKCommand {

public:
	VkResult setContent(MVKCommandBuffer* cmdBuff, Tv value) {
		_value = value;
		return VK_SUCCESS;
	}

protected:
	Tv _value;
};
