/*
 * MVKCmdDispatch.mm
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

#include "MVKCmdDispatch.h"
#include "MVKReplayGPUStages.h"
#include "MVKCommandBuffer.h"
#include "MVKCommandPool.h"
#include "MVKBuffer.h"
#include "MVKPipeline.h"
#include "MVKFoundation.h"
#include "MVKMetalIR.h"
#include "mvk_datatypes.hpp"


#pragma mark -
#pragma mark MVKCmdDispatch

VkResult MVKCmdDispatch::setContent(MVKCommandBuffer* cmdBuff,
									uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ,
									uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
	_baseGroupX = baseGroupX;
	_baseGroupY = baseGroupY;
	_baseGroupZ = baseGroupZ;

	_groupCountX = groupCountX;
	_groupCountY = groupCountY;
	_groupCountZ = groupCountZ;

	return VK_SUCCESS;
}

void MVKCmdDispatch::encode(MVKCommandEncoder* cmdEncoder) {
	MTLRegion mtlThreadgroupCount = MTLRegionMake3D(_baseGroupX, _baseGroupY, _baseGroupZ, _groupCountX, _groupCountY, _groupCountZ);
	auto* pipeline = cmdEncoder->getComputePipeline();
	const auto* artifact = pipeline->getStageResources().metalIR.get();
	if (artifact) {
		mvkir::ComputeData runtime{{_groupCountX, _groupCountY, _groupCountZ}, 0,
			{_baseGroupX, _baseGroupY, _baseGroupZ}};
		cmdEncoder->metalIR().prepareDispatch(artifact, runtime);
	}
	cmdEncoder->finalizeDispatchState();	// Ensure all updated state has been submitted to Metal
	id<MTLComputeCommandEncoder> mtlEncoder = cmdEncoder->getMTLComputeEncoder(kMVKCommandUseDispatch);
	if (pipeline->allowsDispatchBase() && !artifact) {
		// We'll use the stage-input region to pass the base along to the shader.
		// Hopefully Metal won't complain that we didn't set up a stage-input descriptor.
		[mtlEncoder setStageInRegion: mtlThreadgroupCount];
	}
	auto local=pipeline->getThreadgroupSize();
	mvkreplay::noteGPUStageDispatch(cmdEncoder->_mtlCmdBuffer,pipeline->getReplayProgramHash(),_groupCountX,_groupCountY,_groupCountZ,local.width,local.height,local.depth,false);
	[mtlEncoder dispatchThreadgroups: mtlThreadgroupCount.size
			   threadsPerThreadgroup: pipeline->getThreadgroupSize()];
}


#pragma mark -
#pragma mark MVKCmdDispatchIndirect

VkResult MVKCmdDispatchIndirect::setContent(MVKCommandBuffer* cmdBuff, VkBuffer buffer, VkDeviceSize offset) {
	MVKBuffer* mvkBuffer = (MVKBuffer*)buffer;
	_mtlIndirectBuffer = mvkBuffer->getMTLBuffer();
	_mtlIndirectBufferOffset = mvkBuffer->getMTLBufferOffset() + offset;

	return VK_SUCCESS;
}

void MVKCmdDispatchIndirect::encode(MVKCommandEncoder* cmdEncoder) {
    if (const auto* artifact = cmdEncoder->getComputePipeline()->getStageResources().metalIR.get()) {
        cmdEncoder->metalIR().prepareIndirectDispatch(artifact, _mtlIndirectBuffer, _mtlIndirectBufferOffset);
    }
    cmdEncoder->finalizeDispatchState();	// Ensure all updated state has been submitted to Metal
    auto* pipeline=cmdEncoder->getComputePipeline();auto local=pipeline->getThreadgroupSize();
    mvkreplay::noteGPUStageDispatch(cmdEncoder->_mtlCmdBuffer,pipeline->getReplayProgramHash(),0,0,0,local.width,local.height,local.depth,true);
    [cmdEncoder->getMTLComputeEncoder(kMVKCommandUseDispatch) dispatchThreadgroupsWithIndirectBuffer: _mtlIndirectBuffer
																				indirectBufferOffset: _mtlIndirectBufferOffset
																			   threadsPerThreadgroup: cmdEncoder->getComputePipeline()->getThreadgroupSize()];
}
