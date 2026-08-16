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

	virtual bool copyBuffer(MVKBuffer* srcBuffer,
							VkDeviceSize srcOffset,
							MVKBuffer* dstBuffer,
							VkDeviceSize dstOffset,
							VkDeviceSize size) = 0;

	virtual bool fillBuffer(MVKBuffer* dstBuffer,
							VkDeviceSize dstOffset,
							VkDeviceSize size,
							uint8_t value) = 0;
};


#pragma mark -
#pragma mark MVKCommandTypePool

/** A pool of MVKCommand instances of a particular type. */
template <class T>
class MVKCommandTypePool : public MVKObjectPool<T> {

public:

	/** Returns the Vulkan API opaque object controlling this object. */
	MVKVulkanAPIObject* getVulkanAPIObject() override { return nullptr; }

	MVKCommandTypePool(bool isPooling = true) : MVKObjectPool<T>(isPooling) {}

protected:
	T* newObject() override { return new T(); }

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

	/**
	 * Returns whether this command can be materialized by the current Metal 4
	 * backend. Commands are unsupported unless they explicitly opt in.
	 */
	virtual bool supportsMetal4Encoding() const { return false; }

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

