#!/usr/bin/env python3
"""Source contract for the usable Phase 1C Metal 4 compute/transfer/render backend."""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    target = ROOT / path
    if not target.is_file():
        raise AssertionError(f"missing source file: {path}")
    return target.read_text(encoding="utf-8")


def require(source: str, pattern: str, message: str) -> None:
    if not re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


def reject(source: str, pattern: str, message: str) -> None:
    if re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


def function_body(source: str, start: str, end: str) -> str:
    start_index = source.find(start)
    if start_index < 0:
        raise AssertionError(f"missing function: {start}")
    end_index = source.find(end, start_index + len(start))
    if end_index < 0:
        raise AssertionError(f"missing boundary after {start}: {end}")
    return source[start_index:end_index]


def main() -> int:
    command_h = read("MoltenVK/MoltenVK/Commands/MVKCommand.h")
    command_pool_h = read("MoltenVK/MoltenVK/Commands/MVKCommandPool.h")
    command_pool_mm = read("MoltenVK/MoltenVK/Commands/MVKCommandPool.mm")
    command_resource_factory_mm = read(
        "MoltenVK/MoltenVK/Commands/MVKCommandResourceFactory.mm"
    )
    command_buffer_h = read("MoltenVK/MoltenVK/Commands/MVKCommandBuffer.h")
    command_buffer_mm = read("MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm")
    transfer_h = read("MoltenVK/MoltenVK/Commands/MVKCmdTransfer.h")
    transfer_mm = read("MoltenVK/MoltenVK/Commands/MVKCmdTransfer.mm")
    dispatch_h = read("MoltenVK/MoltenVK/Commands/MVKCmdDispatch.h")
    dispatch_mm = read("MoltenVK/MoltenVK/Commands/MVKCmdDispatch.mm")
    draw_h = read("MoltenVK/MoltenVK/Commands/MVKCmdDraw.h")
    draw_mm = read("MoltenVK/MoltenVK/Commands/MVKCmdDraw.mm")
    rendering_h = read("MoltenVK/MoltenVK/Commands/MVKCmdRendering.h")
    rendering_mm = read("MoltenVK/MoltenVK/Commands/MVKCmdRendering.mm")
    viewport_command = function_body(
        rendering_h, "class MVKCmdSetViewport", "#pragma mark MVKCmdSetScissor"
    )
    scissor_command = function_body(
        rendering_h, "class MVKCmdSetScissor", "#pragma mark MVKCmdSetDepthBias"
    )
    pipeline_cmd_h = read("MoltenVK/MoltenVK/Commands/MVKCmdPipeline.h")
    pipeline_cmd_mm = read("MoltenVK/MoltenVK/Commands/MVKCmdPipeline.mm")
    queries_h = read("MoltenVK/MoltenVK/Commands/MVKCmdQueries.h")
    queries_mm = read("MoltenVK/MoltenVK/Commands/MVKCmdQueries.mm")
    pipeline_h = read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h")
    pipeline_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm")
    descriptor_h = read("MoltenVK/MoltenVK/GPUObjects/MVKDescriptorSet.h")
    descriptor_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKDescriptorSet.mm")
    query_pool_h = read("MoltenVK/MoltenVK/GPUObjects/MVKQueryPool.h")
    query_pool_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKQueryPool.mm")
    queue_h = read("MoltenVK/MoltenVK/GPUObjects/MVKQueue.h")
    queue_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm")
    sync_h = read("MoltenVK/MoltenVK/GPUObjects/MVKSync.h")
    sync_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKSync.mm")
    config = read("MoltenVK/MoltenVK/Utility/MVKConfigMembers.def")
    e2e = read("Tests/Metal4CommandBackend/metal4_transfer_e2e.cpp")
    runner = read("Tests/Metal4CommandBackend/run-macos.sh")
    implementation = "\n".join(
        (command_h, command_buffer_h, command_buffer_mm, transfer_h, transfer_mm,
         dispatch_h, dispatch_mm, draw_h, draw_mm, rendering_h, rendering_mm,
         pipeline_cmd_h, pipeline_cmd_mm, queries_h, queries_mm, pipeline_h, pipeline_mm,
         query_pool_h, query_pool_mm, queue_h, queue_mm, sync_h, sync_mm)
    )

    # Private, fail-closed enablement and supported-target boundary.
    require(
        queue_mm,
        r'mvkGetEnvVarNumber\(\s*"MVK_CONFIG_METAL4_COMMAND_BACKEND"\s*,\s*0\.0\s*\)',
        "Metal 4 command backend is not private and default-off",
    )
    reject(
        config,
        r"METAL4_COMMAND_BACKEND|METAL4_COMMAND_ALLOCATOR_COUNT|METAL4_COMMAND_VALIDATION",
        "experimental command controls changed the public MVKConfiguration ABI",
    )
    require(
        implementation,
        r"MVK_XCODE_26\s*&&\s*!MVK_TVOS\s*&&\s*!MVK_VISIONOS\s*&&\s*!MVK_OS_SIMULATOR",
        "Xcode 26/unsupported-target compile boundary is missing",
    )
    require(queue_mm, r"#if\s+MVK_USE_METAL_PRIVATE_API", "private-API build exclusion is missing")
    require(queue_mm, r"supportsMetal4", "Metal 4 device capability gate is missing")
    require(queue_mm, r"mvkOSVersionIsAtLeast\(\s*26\.0\s*\)", "OS 26 gate is missing")

    init_metal4 = function_body(
        queue_mm,
        "void MVKQueue::initMTL4CommandQueue()",
        "MVKQueue::~MVKQueue()",
    )
    reject(
        init_metal4,
        r"getMetalFeatures\(\)\.residencySets",
        "the independent Metal 4 residency set is incorrectly gated by the fork-disabled legacy residency flag",
    )
    reject(
        queue_h,
        r"MVK_CONFIG_METAL4_COMMAND_VALIDATION[\s\S]*?features\.residencySets\s*=\s*true",
        "validation mode can still revive the fork-disabled legacy residency capability",
    )
    require(
        init_metal4,
        r"respondsToSelector:@selector\(newResidencySetWithDescriptor:error:\)",
        "the public Metal 4 residency factory is not probed directly",
    )

    # Use the exact public Xcode 26 descriptor/error factories, not guessed factories.
    for token in (
        "newMTL4CommandQueueWithDescriptor",
        "MTL4CommandQueueDescriptor",
        "newCommandAllocatorWithDescriptor",
        "MTL4CommandAllocatorDescriptor",
        "newCommandBuffer",
        "MTL4CommitOptions",
        "MTL4CommitFeedback",
    ):
        require(queue_mm, re.escape(token), f"missing public Metal 4 factory/lifecycle token: {token}")
    reject(queue_mm, r"\[mtlDevice\s+newMTL4CommandQueue\]", "obsolete queue factory is still used")
    reject(queue_mm, r"\[mtlDevice\s+newCommandAllocator\]", "obsolete allocator factory is still used")
    reject(queue_mm, r"_mtl4Queue\.label\s*=", "read-only MTL4 queue label is assigned after creation")
    reject(queue_mm, r"_computeEncoder\.label\s*=", "unverified writable MTL4 encoder label is assigned")

    # Whole-command-stream preflight, resource resolution, and rollback precede commit.
    require(command_h, r"virtual\s+bool\s+supportsMetal4Encoding\(\)\s+const\s*\{\s*return\s+false", "unsupported commands do not fail closed")
    for token in (
        "prepareMetal4Encoding",
        "beginMetal4Execution",
        "endMetal4Execution",
        "supportsMetal4CommandBuffers",
        "prepareMetal4CommandBuffers",
        "beginMetal4CommandBuffers",
        "endMetal4CommandBuffers",
        "encodeMetal4CommandBuffers",
    ):
        require(implementation, re.escape(token), f"missing whole-submission boundary: {token}")
    require(
        command_buffer_mm,
        r"endMetal4Execution\(bool previousWasExecuted, bool committed\)[\s\S]*?!committed[\s\S]*?_wasExecuted\s*=\s*previousWasExecuted",
        "pre-commit command-buffer claim cannot be rolled back",
    )
    execute_metal4 = function_body(
        queue_mm,
        "VkResult MVKQueueCommandBufferSubmission::executeMetal4(bool* handled)",
        "// Returns the active MTLCommandBuffer",
    )
    require(
        execute_metal4,
        r"supportsMetal4Semaphores\(\)[\s\S]*?supportsMetal4CommandBuffers\([^)]*\)[\s\S]*?prepareMetal4CommandBuffers",
        "submission support is not classified before Metal 4 encoding",
    )
    require(
        execute_metal4,
        r"prepareMetal4CommandBuffers[\s\S]*?acquireResidency[\s\S]*?acquireAllocator[\s\S]*?beginMetal4CommandBuffers[\s\S]*?commit:",
        "resource/allocator/command claims are not ordered before commit",
    )
    require(
        execute_metal4,
        r"@catch[\s\S]*?endMetal4CommandBuffers\(false\)[\s\S]*?return\s+VK_SUCCESS",
        "pre-commit materialization failure cannot select legacy fallback",
    )

    # First real execution slice: buffer/image transfer, barriers, descriptorless compute, and strict basic render.
    for command in ("MVKCmdCopyBuffer", "MVKCmdFillBuffer", "MVKCmdUpdateBuffer"):
        require(transfer_h, rf"{command}[\s\S]*?supportsMetal4Encoding", f"{command} is not opted in")
        require(transfer_h, rf"{command}[\s\S]*?prepareMetal4Encoding", f"{command} does not resolve resources")
        require(transfer_h, rf"{command}[\s\S]*?encodeMetal4", f"{command} does not materialize")
    require(
        transfer_mm,
        r"supportsMetal4CopyBuffer[\s\S]*?regions\.empty\(\)[\s\S]*?mtlCopyBufferAlignment[\s\S]*?srcOffset\s*%\s*alignment[\s\S]*?dstOffset\s*%\s*alignment[\s\S]*?size\s*%\s*alignment",
        "copy eligibility does not reject empty work or preserve Metal alignment rules",
    )
    require(transfer_mm, r"0x01010101u", "fill eligibility does not require a repeated byte")
    require(
        transfer_mm,
        r"MVKCmdUpdateBuffer::setContent[\s\S]*?mtlCopyBufferAlignment[\s\S]*?_dataSize\s*<=\s*65536[\s\S]*?_dstOffset\s*%\s*alignment[\s\S]*?_dataSize\s*%\s*alignment",
        "update-buffer eligibility does not preserve Vulkan size and Metal alignment limits",
    )
    require(
        command_h + queue_mm,
        r"useUpdateBufferData[\s\S]*?newBufferWithBytes[\s\S]*?updateBuffer[\s\S]*?copyFromBuffer",
        "update-buffer staging is not resident and materialized by MTL4",
    )
    require(queue_mm, r"MTL4ComputeCommandEncoder", "real MTL4 compute/transfer encoder is missing")
    require(queue_mm, r"copyFromBuffer:[\s\S]*?destinationOffset:[\s\S]*?size:", "MTL4 buffer copy is missing")
    require(queue_mm, r"fillBuffer:[\s\S]*?range:[\s\S]*?value:", "MTL4 buffer fill is missing")
    require(
        queries_h + queries_mm,
        r"MVKCmdResetQueryPool[\s\S]*?supportsMetal4Encoding",
        "query-pool reset is not materialized by the Metal 4 backend",
    )
    for token in ("MVKCmdResetQueryPool::prepareMetal4Encoding", "MVKCmdResetQueryPool::encodeMetal4"):
        require(queries_mm, re.escape(token), f"query-pool reset is missing: {token}")
    for command in ("MVKCmdBeginQuery", "MVKCmdEndQuery"):
        require(queries_h, rf"{command}[\s\S]*?supportsMetal4Encoding", f"{command} is not opted in")
        require(queries_h + queries_mm, rf"{command}[\s\S]*?prepareMetal4Encoding", f"{command} does not resolve query storage")
        require(queries_h + queries_mm, rf"{command}[\s\S]*?encodeMetal4", f"{command} does not materialize")
    require(
        command_h + queue_mm,
        r"useVisibilityQueryPool[\s\S]*?beginVisibilityQuery[\s\S]*?endVisibilityQuery",
        "visibility-query command surface is missing",
    )
    for token in ("visibilityResultBuffer", "MTLVisibilityResultTypeAccumulate", "setVisibilityResultMode"):
        require(queue_mm, re.escape(token), f"MTL4 render visibility support is missing: {token}")
    require(
        queue_mm + query_pool_h + query_pool_mm,
        r"completedQueries[\s\S]*?applyMetal4End[\s\S]*?finishMetal4Query",
        "query availability is not split between commit and GPU completion",
    )
    require(
        command_h + queue_mm,
        r"useQueryPool[\s\S]*?resetQueryPool[\s\S]*?pendingQueryResets[\s\S]*?publishCommittedState",
        "query-pool reset resources or deferred state publication are missing",
    )
    require(
        query_pool_h + query_pool_mm,
        r"applyMetal4Reset[\s\S]*?resetAvailability",
        "query availability is not reset through the post-commit path",
    )
    require(
        queue_mm,
        r"getMetal4ResetMTLBuffer[\s\S]*?fillBuffer:[\s\S]*?getMetal4ResetRange",
        "occlusion-query result bytes are not cleared by MTL4",
    )
    for token in (
        "MVKCmdCopyImage",
        "supportsMetal4CopyImage",
        "useImage",
        "copyImage",
        "copyFromTexture",
    ):
        require(transfer_h + transfer_mm + queue_mm, re.escape(token), f"MTL4 image copy is missing: {token}")
    require(
        transfer_mm,
        r"supportsMetal4CopyImage[\s\S]*?VK_SAMPLE_COUNT_1_BIT[\s\S]*?getIsCompressed[\s\S]*?needsSwizzle[\s\S]*?VK_IMAGE_ASPECT_COLOR_BIT",
        "image-copy preflight does not fail closed on unsupported formats/aspects",
    )
    require(
        transfer_h + transfer_mm,
        r"MVKCmdBufferImageCopy[\s\S]*?supportsMetal4Encoding[\s\S]*?prepareMetal4Encoding[\s\S]*?encodeMetal4[\s\S]*?copyBufferImage",
        "buffer/image copies are not materialized by the Metal 4 backend",
    )
    require(
        transfer_mm,
        r"getMetal4BufferImageCopyUnsupportedReason[\s\S]*?VK_SAMPLE_COUNT_1_BIT[\s\S]*?MTLTextureType2DArray[\s\S]*?getIsCompressed[\s\S]*?needsSwizzle[\s\S]*?VK_IMAGE_ASPECT_COLOR_BIT[\s\S]*?layerCount\s*!=\s*1[\s\S]*?baseArrayLayer",
        "buffer/image-copy preflight does not fail closed on unsupported formats/aspects",
    )

    for token in (
        "supportsMetal4DescriptorlessExecution",
        "supportsMetal4ArgumentTableExecution",
        "supportsMetal4Execution",
        "MVKCmdBindComputePipeline",
        "useComputePipeline",
        "bindComputePipeline",
        "MVKCmdDispatch",
        "prepareComputeDispatch",
        "dispatchThreadgroups",
        "setComputePipelineState",
    ):
        require(implementation, re.escape(token), f"compute path is missing: {token}")
    require(
        pipeline_h,
        r"supportsMetal4DescriptorlessExecution[\s\S]*?resources\.allBits\.empty[\s\S]*?implicitBuffers\.needed\.empty[\s\S]*?usesPhysicalStorageBufferAddresses",
        "descriptorless compute eligibility does not fail closed on implicit buffers or physical addresses",
    )
    require(
        pipeline_h + pipeline_mm,
        r"supportsMetal4ArgumentTableExecution[\s\S]*?descriptorSetData[\s\S]*?MVKArgumentBufferMode::Metal3",
        "compute pipeline does not expose a fail-closed Metal 3 argument-buffer slice",
    )
    require(
        pipeline_cmd_mm + dispatch_mm + queue_mm,
        r"VK_PIPELINE_BIND_POINT_COMPUTE[\s\S]*?useDescriptorSet[\s\S]*?prepareComputeDispatch[\s\S]*?collectMetal4BindingResources[\s\S]*?setArgumentTable:",
        "compute descriptor sets are not preflighted, retained, and snapshotted into an MTL4 argument table",
    )
    require(
        dispatch_h + dispatch_mm,
        r"_baseGroupX\s*==\s*0[\s\S]*?_baseGroupY\s*==\s*0[\s\S]*?_baseGroupZ\s*==\s*0",
        "MTL4 dispatch accepts unsupported non-zero dispatch bases",
    )

    require(
        pipeline_cmd_h + pipeline_cmd_mm,
        r"MVKCmdPipelineBarrier[\s\S]*?getMetal4PipelineBarrierUnsupportedReason[\s\S]*?prepareMetal4Encoding[\s\S]*?encodeMetal4",
        "buffer/memory pipeline barriers are not materialized",
    )
    require(
        queue_mm,
        r"PendingBarrier[\s\S]*?barrierAfterQueueStages:[\s\S]*?beforeStages:[\s\S]*?visibilityOptions:",
        "cross-encoder MTL4 consumer barrier is missing",
    )
    require(
        queue_mm,
        r"applyRenderAttachmentBarrier[\s\S]*?_previousRenderAttachments[\s\S]*?barrierAfterQueueStages:MTLStageVertex\s*\|\s*MTLStageFragment[\s\S]*?MTL4VisibilityOptionDevice",
        "reused render attachments are not made visible between MTL4 render passes",
    )
    reject(
        queue_mm,
        r"_computeEncoder\s+barrierAfterEncoderStages:[\s\S]*?MTLStageVertex|_renderEncoder\s+barrierAfterEncoderStages:[\s\S]*?MTLStageBlit",
        "render/compute stage masks are passed to an incompatible intra-pass encoder barrier",
    )
    require(
        pipeline_cmd_mm,
        r"dependencyFlags\s*!=\s*0[\s\S]*?barrier_dependency_flags[\s\S]*?_supportsMetal4Encoding\s*=\s*!_metal4UnsupportedReason",
        "unsupported dependency flags do not fail closed",
    )
    require(
        command_h + pipeline_cmd_mm + queue_mm,
        r"trackImageBarrier[\s\S]*?pendingImageBarriers[\s\S]*?publishCommittedState",
        "image-layout transitions are not deferred until a successful Metal 4 commit",
    )
    require(
        pipeline_cmd_mm,
        r"getMetal4PipelineBarrierUnsupportedReason[\s\S]*?VK_PIPELINE_STAGE_2_HOST_BIT[\s\S]*?VK_ACCESS_2_HOST_READ_BIT[\s\S]*?barrier_host_access",
        "host-read barrier side effects do not fail closed",
    )
    require(
        pipeline_cmd_mm,
        r"srcIgnored\s*&&\s*dstIgnored[\s\S]*?srcQueueFamilyIndex\s*==\s*barrier\.dstQueueFamilyIndex",
        "queue-family ownership barriers do not fail closed on a half-ignored transfer",
    )
    require(
        pipeline_cmd_mm,
        r"barrier\.type\s*==\s*MVKPipelineBarrier::Image[\s\S]*?useImage\(barrier\.mvkImage\)",
        "image barriers do not register their Metal textures before execution is claimed",
    )
    require(
        pipeline_cmd_mm,
        r"MVKCmdPipelineBarrier<[^>]+>::encodeMetal4[\s\S]*?pipelineBarrier\(",
        "image/buffer/memory barriers are not encoded through the backend-neutral barrier boundary",
    )
    require(
        pipeline_cmd_h + pipeline_cmd_mm,
        r"getMetal4UnsupportedReason[\s\S]*?barrier_dependency_flags[\s\S]*?barrier_missing_type[\s\S]*?barrier_host_access[\s\S]*?getMetal4ImageLayoutUnsupportedReason[\s\S]*?barrier_image_range[\s\S]*?barrier_queue_family",
        "Metal 4 barrier fallbacks are not split into actionable reason groups",
    )
    require(
        pipeline_cmd_mm,
        r"getMetal4ImageLayoutUnsupportedReason[\s\S]*?barrier_image_layout_general[\s\S]*?barrier_image_layout_shader_read[\s\S]*?barrier_image_layout_depth_stencil_read[\s\S]*?barrier_image_layout_present[\s\S]*?barrier_image_layout_undefined[\s\S]*?barrier_image_layout_other",
        "unsupported Metal 4 image layouts are not classified before widening support",
    )
    require(
        pipeline_cmd_mm,
        r"generalAspects[\s\S]*?VK_IMAGE_ASPECT_COLOR_BIT[\s\S]*?VK_IMAGE_ASPECT_DEPTH_BIT[\s\S]*?VK_IMAGE_ASPECT_STENCIL_BIT[\s\S]*?generalLayout[\s\S]*?VK_IMAGE_LAYOUT_GENERAL",
        "the validated GENERAL image-layout barrier slice is not enabled",
    )
    require(
        e2e + runner,
        r"GENERAL_LAYOUT_IMAGE_OK[\s\S]*?GENERAL_LAYOUT_IMAGE_OK",
        "GENERAL image-layout barriers lack a data-validating E2E gate",
    )
    require(
        pipeline_cmd_mm + e2e + runner,
        r"presentLayout[\s\S]*?VK_IMAGE_LAYOUT_PRESENT_SRC_KHR[\s\S]*?VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR[\s\S]*?vkCreateHeadlessSurfaceEXT[\s\S]*?vkQueuePresentKHR[\s\S]*?PRESENT_LAYOUT_AND_QUEUE_OK[\s\S]*?PRESENT_LAYOUT_AND_QUEUE_OK",
        "present-layout barriers are not handed from a real Metal 4 submission to queue presentation",
    )

    # Strict ordinary-render slice: one dynamic-rendering color attachment, an optional
    # single-sample depth attachment, a descriptorless graphics pipeline, and a real
    # non-indexed draw on MTL4RenderCommandEncoder.
    for token in (
        "useImageView",
        "useGraphicsPipeline",
        "beginRendering",
        "endRendering",
        "bindGraphicsPipeline",
        "draw",
    ):
        require(command_h, re.escape(token), f"backend-neutral render boundary is missing: {token}")
    require(
        rendering_h + rendering_mm,
        r"MVKCmdBeginRendering[\s\S]*?supportsMetal4Encoding[\s\S]*?prepareMetal4Encoding[\s\S]*?encodeMetal4",
        "dynamic rendering is not opted into the strict Metal 4 render slice",
    )
    require(
        rendering_mm,
        r"mvkSupportsMetal4RenderingAttachment[\s\S]*?VK_SAMPLE_COUNT_1_BIT[\s\S]*?mvkSupportsMetal4RenderingInfo[\s\S]*?colorAttachmentCount\s*==\s*0[\s\S]*?kMVKMaxColorAttachmentCount[\s\S]*?pStencilAttachment[\s\S]*?pDepthAttachment",
        "dynamic-rendering eligibility does not fail closed on unsupported attachments or multisampling",
    )
    require(
        rendering_mm,
        r"prepareMetal4Encoding[\s\S]*?pDepthAttachment[\s\S]*?useImageView",
        "depth attachment residency is not prepared before Metal 4 encoding",
    )
    require(
        queue_mm,
        r"descriptor\.depthAttachment[\s\S]*?clearDepth",
        "depth attachments are not materialized on the Metal 4 render encoder",
    )
    require(
        queue_mm,
        r"newDepthStencilStateWithDescriptor[\s\S]*?setDepthStencilState",
        "static Vulkan depth state is not bound to the Metal 4 render encoder",
    )
    require(
        viewport_command,
        r"supportsMetal4Encoding[\s\S]*?encodeMetal4",
        "dynamic viewport commands are not materialized on Metal 4",
    )
    require(
        rendering_mm,
        r"MVKCmdSetViewport<N>::encodeMetal4[\s\S]*?setViewports",
        "dynamic viewport commands do not reach the Metal 4 encoder",
    )
    require(
        scissor_command,
        r"supportsMetal4Encoding[\s\S]*?encodeMetal4",
        "dynamic scissor commands are not materialized on Metal 4",
    )
    require(
        rendering_mm,
        r"MVKCmdSetScissor<N>::encodeMetal4[\s\S]*?setScissors",
        "dynamic scissor commands do not reach the Metal 4 encoder",
    )
    require(
        command_h + queue_mm,
        r"setViewports[\s\S]*?setScissors[\s\S]*?setViewports:[\s\S]*?setScissorRects:",
        "the Metal 4 encoder boundary does not apply dynamic viewport and scissor state",
    )
    require(
        rendering_h + rendering_mm,
        r"MVKCmdSetStencilCompareMask[\s\S]*?encodeMetal4[\s\S]*?MVKCmdSetStencilWriteMask[\s\S]*?encodeMetal4[\s\S]*?MVKCmdSetStencilReference[\s\S]*?encodeMetal4",
        "inert dynamic stencil commands are not materialized on Metal 4",
    )
    require(
        pipeline_mm,
        r"StencilTestEnable[\s\S]*?stencilTestEnabled[\s\S]*?removeAll\(\{[\s\S]*?StencilCompareMask[\s\S]*?StencilWriteMask[\s\S]*?StencilReference",
        "disabled stencil pipelines retain semantically inert dynamic stencil masks",
    )
    require(
        pipeline_mm,
        r"DepthBiasEnable[\s\S]*?MVKRenderStateEnableFlag::DepthBias[\s\S]*?needed\.remove\(MVKRenderStateFlag::DepthBias\)",
        "disabled depth-bias pipelines retain a semantically inert dynamic depth-bias value",
    )
    require(
        rendering_h + rendering_mm,
        r"MVKCmdSetDepthBias[\s\S]*?supportsMetal4Encoding[\s\S]*?encodeMetal4[\s\S]*?return cmdEncoder",
        "the inert dynamic depth-bias command is not accepted by Metal 4 preflight",
    )
    require(
        pipeline_cmd_h + pipeline_cmd_mm,
        r"MVKCmdBindGraphicsPipeline[\s\S]*?supportsMetal4RenderExecution[\s\S]*?useGraphicsPipeline[\s\S]*?bindGraphicsPipeline",
        "eligible graphics-pipeline binding is not materialized",
    )
    require(
        pipeline_h,
        r"supportsMetal4DescriptorlessRenderExecution",
        "graphics pipeline does not expose strict Metal 4 render eligibility",
    )
    require(
        pipeline_h + read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm"),
        r"_supportsMetal4DescriptorlessRenderExecution[\s\S]*?resources\.allBits\.empty[\s\S]*?implicitBuffers\.needed\.empty",
        "graphics pipeline eligibility does not reject descriptor or implicit-buffer use",
    )
    require(
        pipeline_mm,
        r"kMetal4SupportedDynamicState[\s\S]*?VertexStride[\s\S]*?Viewports[\s\S]*?Scissors[\s\S]*?BlendConstants",
        "the supported Metal 4 dynamic-state set is incomplete",
    )
    require(
        rendering_h + rendering_mm + queue_mm,
        r"MVKCmdSetBlendConstants[\s\S]*?encodeMetal4[\s\S]*?setBlendConstants[\s\S]*?setBlendColorRed:[\s\S]*?green:[\s\S]*?blue:[\s\S]*?alpha:",
        "dynamic blend constants are not preserved through Metal 4 draw encoding",
    )
    require(
        pipeline_h + pipeline_mm + rendering_mm + queue_mm + e2e,
        r"getMetal4ColorAttachmentCount[\s\S]*?kMVKMaxColorAttachmentCount[\s\S]*?colorAttachmentCount[\s\S]*?colorAttachments\[colorIndex\][\s\S]*?CLASSIC_MRT_RENDER_OK",
        "classic Metal 4 MRT does not preserve pipeline and render-pass attachment arrays",
    )
    require(
        transfer_h + transfer_mm + command_h + queue_mm + e2e + runner,
        r"MVKCmdClearAttachments[\s\S]*?supportsMetal4Encoding[\s\S]*?useClearAttachments[\s\S]*?getCmdClearMTLRenderPipelineState[\s\S]*?clearAttachments[\s\S]*?drawPrimitives:[\s\S]*?CLASSIC_CLEAR_ATTACHMENTS_OK",
        "classic vkCmdClearAttachments is not preflighted, materialized, and read back on Metal 4",
    )
    require(
        rendering_h + queue_mm,
        r"MVKCmdSetViewport[\s\S]*?kMVKMaxViewportScissorCount[\s\S]*?setViewports:[\s\S]*?count:viewportCount[\s\S]*?setScissorRects:[\s\S]*?count:scissorCount",
        "Metal 4 dynamic viewport/scissor arrays are not preserved through draw encoding",
    )
    require(
        pipeline_h + pipeline_mm,
        r"hasSupportedVertexInput[\s\S]*?_translatedVertexBindings\.empty\(\)[\s\S]*?_zeroDivisorVertexBindings\.empty\(\)[\s\S]*?removingAll\(kMetal4SupportedDynamicState\)",
        "render eligibility does not isolate supported Metal 4 dynamic state",
    )
    require(
        pipeline_h + pipeline_mm + e2e + runner,
        r"isRasterizationDisabled[\s\S]*?rasterizationEnabled[\s\S]*?hasVertexOnlyDiscardStage[\s\S]*?!_isTessellationPipeline[\s\S]*?RASTERIZER_DISCARD_OK",
        "vertex-only statically disabled rasterization is not executed and read back through Metal 4",
    )
    require(
        pipeline_mm + queue_mm + e2e,
        r"isMetal4SupportedStaticPrimitiveTopology[\s\S]*?VK_PRIMITIVE_TOPOLOGY_LINE_LIST[\s\S]*?stateData\.primitiveType[\s\S]*?RASTERIZER_DISCARD_OK",
        "static non-triangle primitive topology is not mapped into Metal 4 draws",
    )
    require(
        pipeline_mm,
        r"getMetal4UnsupportedDynamicStateReason[\s\S]*?removingAll\(kMetal4SupportedDynamicState\)",
        "dynamic-state telemetry still reports an already-supported Metal 4 state",
    )
    require(
        pipeline_mm,
        r"getMetal4UnsupportedFixedFunctionReason[\s\S]*?fixed_function_topology[\s\S]*?fixed_function_rasterization[\s\S]*?fixed_function_multisample[\s\S]*?fixed_function_viewport[\s\S]*?fixed_function_depth_stencil",
        "fixed-function telemetry is not split into actionable eligibility groups",
    )
    require(
        draw_h + draw_mm,
        r"MVKCmdBindVertexBuffers[\s\S]*?supportsMetal4Encoding[\s\S]*?prepareMetal4Encoding[\s\S]*?useBuffer[\s\S]*?encodeMetal4[\s\S]*?bindVertexBuffers",
        "vertex-buffer binding is not preflighted and materialized",
    )
    require(
        queue_mm,
        r"getVkVertexBuffers[\s\S]*?getMetalBufferIndexForVertexAttributeBinding[\s\S]*?setAddress:[\s\S]*?MTLRenderStageVertex",
        "static vertex buffers are not mapped into the Metal 4 argument table",
    )
    require(
        pipeline_h + queue_mm,
        r"usesMetal4DynamicVertexStride[\s\S]*?supportAttributeStrides\s*=\s*YES[\s\S]*?setAddress:[\s\S]*?attributeStride:",
        "dynamic vertex stride is not snapshotted into the Metal 4 argument table",
    )
    require(
        pipeline_h + pipeline_mm,
        r"supportsMetal4ArgumentTableRenderExecution[\s\S]*?stageSupportsArgumentTable[\s\S]*?descriptorSetData[\s\S]*?MVKArgumentBufferMode::Metal3",
        "graphics pipeline does not expose a fail-closed Metal 3 argument-buffer slice",
    )
    require(
        pipeline_cmd_h + pipeline_cmd_mm,
        r"MVKCmdBindDescriptorSetsStatic[\s\S]*?supportsMetal4Encoding[\s\S]*?prepareMetal4Encoding[\s\S]*?useDescriptorSet[\s\S]*?encodeMetal4[\s\S]*?bindDescriptorSets",
        "static descriptor-set binding is not preflighted and materialized",
    )
    require(
        pipeline_cmd_h,
        r"MVKCmdBindDescriptorSetsDynamic[\s\S]*?supportsMetal4Encoding\(\) const override \{ return false; \}",
        "dynamic descriptor offsets must remain fail closed",
    )
    require(
        descriptor_h + descriptor_mm,
        r"supportsMetal4ArgumentTable[\s\S]*?MVKArgumentBufferMode::Metal3[\s\S]*?UPDATE_AFTER_BIND[\s\S]*?UPDATE_UNUSED_WHILE_PENDING[\s\S]*?VARIABLE_DESCRIPTOR_COUNT[\s\S]*?PARTIALLY_BOUND",
        "descriptor-set eligibility does not reject mutable, variable-count, or partially-bound layouts",
    )
    require(
        pipeline_h + pipeline_mm + descriptor_h + descriptor_mm + queue_mm,
        r"descriptorBindings[\s\S]*?collectMetal4BindingResources[\s\S]*?prepareGraphicsDraw[\s\S]*?retainDescriptorAllocation[\s\S]*?_allocations\.push_back",
        "only statically used descriptor bindings are not retained and made resident before Metal 4 execution",
    )
    require(
        queue_mm,
        r"MTL4ArgumentTableDescriptor[\s\S]*?newArgumentTableWithDescriptor[\s\S]*?setAddress:[\s\S]*?setArgumentTable:",
        "Metal 3 argument buffers are not snapshotted through an MTL4 argument table",
    )
    require(
        draw_h + draw_mm,
        r"MVKCmdDraw[\s\S]*?supportsMetal4Encoding[\s\S]*?prepareMetal4Encoding[\s\S]*?encodeMetal4[\s\S]*?cmdEncoder->draw",
        "real non-indexed Vulkan draw is not routed to Metal 4",
    )
    require(
        draw_h + draw_mm + command_h,
        r"MVKCmdBindIndexBuffer[\s\S]*?supportsMetal4Encoding[\s\S]*?prepareMetal4Encoding[\s\S]*?useBuffer[\s\S]*?encodeMetal4[\s\S]*?bindIndexBuffer[\s\S]*?MVKCmdDrawIndexed[\s\S]*?supportsMetal4Encoding[\s\S]*?encodeMetal4[\s\S]*?drawIndexed",
        "indexed Vulkan draws are not preflighted and routed to Metal 4",
    )
    require(
        queue_mm,
        r"bindIndexBuffer[\s\S]*?VK_INDEX_TYPE_UINT16[\s\S]*?VK_INDEX_TYPE_UINT32[\s\S]*?_boundIndexBuffer[\s\S]*?drawIndexedPrimitives:[\s\S]*?indexBufferLength:[\s\S]*?baseVertex:[\s\S]*?baseInstance:",
        "Metal 4 does not validate and encode bounded uint16/uint32 indexed draws",
    )
    for token in (
        "MTL4RenderPassDescriptor",
        "MTL4ArgumentTable",
        "renderCommandEncoderWithDescriptor",
        "setRenderPipelineState",
        "setViewport",
        "setScissorRect",
        "drawPrimitives",
        "bindVertexBuffers",
        "recordRenderSubmission",
        "recordRenderPass",
        "recordDraw",
    ):
        require(queue_mm, re.escape(token), f"Metal 4 render materializer is missing: {token}")

    # Independent residency remains live until the definitive completion callback.
    for token in (
        "MTLResidencySetDescriptor",
        "addAllocation",
        "removeAllocation",
        "acquireResidency",
        "releaseResidency",
        "inFlightCount",
        "resetPending",
    ):
        require(queue_mm, re.escape(token), f"residency/allocator lifetime is incomplete: {token}")
    require(
        queue_mm,
        r"MVKMetal4SubmissionCompletion[\s\S]*?completeAllocator[\s\S]*?releaseResidency[\s\S]*?finish\(\)",
        "completion does not release allocator/residency before Vulkan fence completion",
    )
    require(
        queue_mm,
        r"schedulingComplete[\s\S]*?completionRequested",
        "an early MTL4 callback can destroy a submission while queue scheduling is still running",
    )
    require(
        queue_mm,
        r"receiveFeedback[\s\S]*?hostSignalOrdering\(sequence\)[\s\S]*?complete\(\)",
        "workload-completion feedback is not the authoritative release boundary",
    )
    probe_wait = function_body(
        queue_mm,
        "bool waitForProbe(uint64_t timeoutNs)",
        "void recordRealSubmission",
    )
    reject(
        probe_wait,
        r"completeAllocator",
        "a probe timeout can reset an allocator whose workload may still be in flight",
    )
    require(
        queue_mm,
        r"atomic<bool>\s+probeAllocatorCompleted\s*=\s*false[\s\S]*?probeAllocatorCompleted\.exchange[\s\S]*?completeAllocator",
        "late probe feedback lacks a declared idempotent allocator release guard",
    )
    acquire_allocator = function_body(
        queue_mm,
        "bool acquireAllocator(size_t* slotIndex, id<MTL4CommandAllocator>* allocator)",
        "void finishEncoding",
    )
    require(
        acquire_allocator,
        r"nextAllocatorIndex[\s\S]*?inFlightCount\s*!=\s*0[\s\S]*?continue",
        "allocator selection is not bounded round-robin or reuses in-flight allocator memory",
    )
    finish_encoding = function_body(
        queue_mm,
        "void finishEncoding(size_t slotIndex, bool submitted)",
        "void completeAllocator",
    )
    require(
        finish_encoding,
        r"!submitted[\s\S]*?inFlightCount\s*==\s*0[\s\S]*?\[slot\.allocator\s+reset\]",
        "a safely ended unsubmitted command buffer does not recycle its allocator heaps",
    )
    require(
        execute_metal4,
        r"queueSideEffectsStarted\s*=\s*true[\s\S]*?waitForEvent:[\s\S]*?@catch[\s\S]*?if\s*\(!queueSideEffectsStarted\s*&&\s*!commitAttempted\)",
        "legacy replay remains possible after a Metal 4 queue wait or commit attempt",
    )
    require(
        execute_metal4,
        r"commitAttempted\s*=\s*true[\s\S]*?commit:[\s\S]*?@catch[\s\S]*?if\s*\(!commitAttempted\)[\s\S]*?endMetal4CommandBuffers\(false\)",
        "pre-commit exceptions are not separated from ambiguous post-commit failures",
    )
    require(
        execute_metal4,
        r"commandBufferBeginAttempted[\s\S]*?beginCommandBufferWithAllocator:[\s\S]*?encoderEndAttempted[\s\S]*?endEncoding\(\)[\s\S]*?commandBufferEndAttempted[\s\S]*?endCommandBuffer",
        "Metal 4 command-buffer begin/end attempts are not tracked for safe allocator reuse",
    )
    require(
        queue_mm,
        r"bool\s+retired\s*=\s*false[\s\S]*?slot\.retired[\s\S]*?continue[\s\S]*?void\s+retireAllocator",
        "an allocator with an ambiguous command-buffer lifetime cannot be retired",
    )
    require(
        execute_metal4,
        r"abandonEncoding\(\)[\s\S]*?retireAllocator\(allocatorIndex\)[\s\S]*?VK_ERROR_DEVICE_LOST",
        "failed command-buffer cleanup can reuse an unsafe allocator or replay the submission",
    )
    submission_probe = function_body(
        queue_mm,
        "bool MVKQueue::startMTL4CommandSubmissionProbe()",
        "// Creates the independent Metal 4 queue",
    )
    require(
        submission_probe,
        r"commandBufferBeginAttempted[\s\S]*?beginCommandBufferWithAllocator:[\s\S]*?commandBufferEndAttempted[\s\S]*?endCommandBuffer[\s\S]*?retireAllocator",
        "the startup probe can reuse an allocator after an ambiguous command-buffer lifetime",
    )
    require(
        submission_probe,
        r"probeMayBeInFlight\.store\(true[\s\S]*?commit:commandBuffers",
        "the startup probe does not mark a commit attempt as potentially in flight",
    )
    require(
        queue_mm,
        r"void\s+completeProbe\([^)]*\)[\s\S]*?probeMayBeInFlight\.store\(false",
        "Metal feedback does not definitively clear the startup probe's in-flight state",
    )
    require(
        init_metal4,
        r"startMTL4CommandSubmissionProbe\(\)[\s\S]*?probeMayBeInFlight[\s\S]*?retaining its Metal 4 sidecar[\s\S]*?_metal4CommandState\.reset\(\)",
        "ambiguous probe failure can release the queue/state before Metal feedback",
    )
    ambiguous_start = execute_metal4.find("Metal 4 Vulkan queue commit became ambiguous")
    ambiguous_end = execute_metal4.find("[orderingEvent release]", ambiguous_start)
    if ambiguous_start < 0 or ambiguous_end < 0:
        raise AssertionError("ambiguous post-commit boundary is missing")
    ambiguous_catch = execute_metal4[ambiguous_start:ambiguous_end]
    reject(
        ambiguous_catch,
        r"hostSignalOrdering\(_submissionSequence\)|completion->complete\(\)|completeAllocator\(allocatorIndex\)|releaseResidency\(allocations\)",
        "ambiguous post-commit failure releases or advances work before real Metal feedback",
    )
    require(
        queue_mm,
        r"struct\s+CommandCounters[\s\S]*?publishCommittedCounters",
        "command telemetry is not accumulated per submission",
    )
    require(
        execute_metal4,
        r"commit:[\s\S]*?publishCommittedState[\s\S]*?publishCommittedCounters",
        "Metal 4 state or telemetry is published before a successful commit",
    )
    for token in (
        "MVKMetal4FallbackReason",
        "unsupported_semaphore",
        "unsupported_command_buffer",
        "prepare_failed",
        "residency_acquire_failed",
        "allocator_unavailable",
        "command_object_unavailable",
        "encoding_replayable_exception",
        "command_buffer_not_ended",
        "precommit_replayable_exception",
        "attemptedSubmissionCount",
        "recordSubmissionAttempt",
        "fallbackReasonCounts",
        "Metal 4 command backend live",
    ):
        require(queue_mm, re.escape(token), f"live command-backend telemetry is missing: {token}")
    require(
        execute_metal4,
        r"recordSubmissionAttempt\(\)[\s\S]*?recordFallback\([^)]*MVKMetal4FallbackReason",
        "Metal 4 attempts and classified fallbacks are not recorded at runtime",
    )
    require(
        execute_metal4,
        r"totalCount\s*&\s*\(fallback\.totalCount\s*-\s*1\)",
        "fallback snapshots are not rate-limited to power-of-two totals",
    )
    require(
        command_h + command_buffer_mm + queue_mm,
        r"recordMetal4EncodingFailure[\s\S]*?getMetal4CommandTypeName[\s\S]*?Metal 4 command materialization failed for %s",
        "replayable materialization failures do not identify the concrete Vulkan command",
    )
    for token in (
        "begin_query_pool_mismatch",
        "begin_query_already_active",
        "dispatch_compute_encoder_unavailable",
        "dispatch_pipeline_unbound",
        "dispatch_resources_unavailable",
        "clear_attachments_active_query",
        "draw_indexed_pipeline_incompatible",
    ):
        require(
            queue_mm,
            re.escape(token),
            f"query and dispatch materialization telemetry is missing: {token}",
        )
    require(
        queue_mm,
        r"Metal 4 command backend summary: attempts=%llu, real_submissions=%llu",
        "final command-backend summary does not include total attempts",
    )
    for source, pattern, message in (
        (
            command_h,
            r"getMetal4CommandTypeName\(\)\s+const",
            "commands do not expose stable Metal 4 telemetry identity",
        ),
        (
            command_pool_h + command_pool_mm,
            r"#cmdType",
            "macro-generated command pools do not assign stable command type names",
        ),
        (
            command_buffer_h + command_buffer_mm,
            r"supportsMetal4Encoding\([^)]*firstUnsupportedCommand",
            "command-buffer preflight does not report the first unsupported command",
        ),
        (
            queue_mm,
            r"kMetal4UnsupportedCommandCapacity",
            "unsupported-command telemetry is not explicitly bounded",
        ),
        (
            queue_mm,
            r"recordUnsupportedCommand",
            "unsupported-command telemetry is not aggregated",
        ),
        (
            execute_metal4,
            r"latest_unsupported_command=%s[\s\S]*?unsupported_commands=%s",
            "live fallback telemetry does not identify the command blocking Metal 4",
        ),
        (
            command_h + command_buffer_mm,
            r"getMetal4UnsupportedReason",
            "unsupported commands cannot refine the bounded blocker reason",
        ),
        (
            pipeline_h + pipeline_cmd_mm + pipeline_mm,
            r"metal4RenderExecutionUnsupportedReason[\s\S]*?descriptor_resources[\s\S]*?vertex_input[\s\S]*?attachment_render_pass_mrt[\s\S]*?attachment_render_pass_stencil_active[\s\S]*?attachment_render_pass_stencil_inactive[\s\S]*?attachment_rendering_info[\s\S]*?attachment_multiview[\s\S]*?attachment_color_count[\s\S]*?attachment_color_format[\s\S]*?attachment_stencil[\s\S]*?attachment_depth_format[\s\S]*?attachment_depth_state",
            "graphics-pipeline blockers are not classified before expanding the render backend",
        ),
        (
            pipeline_mm,
            r"hasActiveStencilState[\s\S]*?StencilTestEnable[\s\S]*?stencilTestEnabled",
            "classic render-pass stencil telemetry does not distinguish active stencil testing from an inert attachment",
        ),
        (
            pipeline_h + pipeline_mm,
            r"getMetal4StencilAttachmentFormat[\s\S]*?_metal4StencilAttachmentFormat[\s\S]*?hasSupportedClassicStencilAttachment",
            "graphics pipelines do not retain a fail-closed classic inactive-stencil format contract",
        ),
        (
            queue_mm,
            r"descriptor\.stencilAttachment\s*=\s*legacyDescriptor\.stencilAttachment[\s\S]*?_currentStencilFormat[\s\S]*?getMetal4StencilAttachmentFormat",
            "classic inactive-stencil render passes are not bound and format-validated on the Metal 4 encoder",
        ),
        (
            queue_mm,
            r"depthTestEnabled[\s\S]*?MVKRenderStateEnableFlag::DepthTest[\s\S]*?MTLCompareFunctionAlways[\s\S]*?depthWriteEnabled\s*=\s*depthTestEnabled",
            "Metal 4 does not neutralize depth compare and writes when Vulkan depth testing is disabled",
        ),
        (
            queue_mm,
            r"stencilTestEnabled[\s\S]*?frontFaceStencilData[\s\S]*?backFaceStencilData[\s\S]*?frontFaceStencil[\s\S]*?backFaceStencil[\s\S]*?setStencilFrontReferenceValue",
            "Metal 4 does not bind the complete static front/back stencil state and reference values",
        ),
        (
            pipeline_mm,
            r"hasSupportedStencilAttachment[\s\S]*?isStencilFormat[\s\S]*?hasStrictFixedFunction[\s\S]*?hasSupportedDepthState;",
            "static stencil pipelines are not eligible while dynamic stencil state remains fail closed",
        ),
        (
            rendering_mm,
            r"STENCIL_ATTACHMENT_OPTIMAL[\s\S]*?pStencilAttachment[\s\S]*?mvkSupportsMetal4RenderingAttachment[\s\S]*?useImageView",
            "dynamic rendering does not validate and retain its Metal 4 stencil attachment",
        ),
        (
            queue_mm,
            r"renderingInfo\.pStencilAttachment[\s\S]*?descriptor\.stencilAttachment[\s\S]*?clearStencil[\s\S]*?_currentStencilFormat",
            "dynamic rendering does not bind and format-track its Metal 4 stencil attachment",
        ),
        (
            pipeline_h + pipeline_mm,
            r"usesMetal4DynamicStencilCompareMask[\s\S]*?usesMetal4DynamicStencilWriteMask[\s\S]*?usesMetal4DynamicStencilReference[\s\S]*?kMetal4SupportedDynamicState[\s\S]*?StencilCompareMask[\s\S]*?StencilWriteMask[\s\S]*?StencilReference",
            "the Metal 4 render slice does not admit Ryujinx's three dynamic stencil values",
        ),
        (
            queue_mm,
            r"setStencilCompareMask[\s\S]*?_dynamicStencilCompareMask[\s\S]*?setStencilWriteMask[\s\S]*?_dynamicStencilWriteMask[\s\S]*?setStencilReference[\s\S]*?_dynamicStencilReference[\s\S]*?newDepthStencilStateWithDescriptor[\s\S]*?setStencilFrontReferenceValue",
            "Metal 4 does not materialize dynamic stencil masks and references at draw time",
        ),
        (
            pipeline_mm,
            r"dynamic_viewport_scissor[\s\S]*?dynamic_depth_stencil[\s\S]*?dynamic_rasterization[\s\S]*?dynamic_topology[\s\S]*?dynamic_color_blend[\s\S]*?dynamic_sampling[\s\S]*?dynamic_tessellation[\s\S]*?dynamic_other",
            "dynamic-state blockers are not split into actionable capability groups",
        ),
        (
            rendering_h + rendering_mm,
            r"MVKCmdBeginRenderPass[\s\S]*?getMetal4UnsupportedReason[\s\S]*?_metal4UnsupportedReason[\s\S]*?classic_render_pass_missing[\s\S]*?classic_render_pass_subpass_count[\s\S]*?classic_render_pass_secondary_contents[\s\S]*?classic_render_pass_missing_framebuffer[\s\S]*?classic_render_pass_layer_count[\s\S]*?classic_render_pass_partial_area[\s\S]*?classic_render_pass_multiview[\s\S]*?classic_render_pass_multisample[\s\S]*?classic_render_pass_color_count[\s\S]*?classic_render_pass_no_color[\s\S]*?classic_render_pass_unused_color[\s\S]*?classic_render_pass_attachment_missing[\s\S]*?classic_render_pass_attachment_plane_count[\s\S]*?classic_render_pass_attachment_multisample[\s\S]*?classic_render_pass_attachment_texture_type[\s\S]*?classic_render_pass_attachment_layer_count[\s\S]*?classic_render_pass_attachment_swizzle[\s\S]*?classic_render_pass_attachment_extent",
            "classic render-pass fallback telemetry does not identify the actual unsupported condition",
        ),
        (
            rendering_mm + queue_mm + pipeline_mm + e2e,
            r"!hasUsedColorAttachment[\s\S]*?isDepthAttachmentUsed[\s\S]*?isStencilAttachmentUsed[\s\S]*?hasSupportedRenderAttachments[\s\S]*?colorAttachmentCount\s*==\s*0[\s\S]*?depthAttachmentFormat\s*!=\s*VK_FORMAT_UNDEFINED[\s\S]*?stencilAttachmentFormat\s*!=\s*VK_FORMAT_UNDEFINED[\s\S]*?CLASSIC_DEPTH_ONLY_RENDER_OK",
            "classic depth-only render passes are not admitted by the command, descriptor, pipeline, and E2E path",
        ),
        (
            rendering_mm + queue_mm + pipeline_h + pipeline_mm + e2e,
            r"MVKCmdSetDepthBias::encodeMetal4[\s\S]*?setDepthBias[\s\S]*?usesMetal4DynamicDepthBias[\s\S]*?MVKRenderStateFlag::DepthBias[\s\S]*?ACTIVE_DYNAMIC_DEPTH_BIAS_OK",
            "active dynamic depth bias is not retained and materialized by the Metal 4 render path",
        ),
        (
            transfer_mm + queue_mm + e2e,
            r"MVKCmdClearAttachments<N>::setContent[\s\S]*?if \(renderingInfo\)[\s\S]*?dynamic_clear_flags[\s\S]*?useClearAttachments[\s\S]*?DYNAMIC_CLEAR_ATTACHMENTS_OK",
            "dynamic-rendering single attachment clears are not admitted and exercised on Metal 4",
        ),
        (
            transfer_h + transfer_mm,
            r"MVKCmdClearAttachments[\s\S]*?getMetal4UnsupportedReason[\s\S]*?_metal4UnsupportedReason[\s\S]*?dynamic_clear_flags[\s\S]*?dynamic_clear_multiview[\s\S]*?dynamic_clear_layer_count[\s\S]*?dynamic_clear_rect[\s\S]*?dynamic_clear_color_attachment_missing[\s\S]*?dynamic_clear_multisample[\s\S]*?dynamic_clear_depth_stencil_attachment_missing[\s\S]*?classic_clear_framebuffer_missing[\s\S]*?classic_clear_multisample[\s\S]*?classic_clear_framebuffer_layer_count[\s\S]*?classic_clear_layer_count[\s\S]*?classic_clear_negative_offset[\s\S]*?classic_clear_zero_extent",
            "clear-attachment fallback telemetry does not identify the actual unsupported shape",
        ),
        (
            transfer_h + transfer_mm,
            r"MVKCmdBufferImageCopy[\s\S]*?getMetal4UnsupportedReason[\s\S]*?_metal4UnsupportedReason[\s\S]*?buffer_image_layout[\s\S]*?buffer_image_multisample[\s\S]*?buffer_image_multiplane[\s\S]*?buffer_image_texture_type[\s\S]*?buffer_image_compressed[\s\S]*?buffer_image_swizzle[\s\S]*?buffer_image_texel_size[\s\S]*?buffer_image_aspect[\s\S]*?buffer_image_base_layer[\s\S]*?buffer_image_layer_count[\s\S]*?buffer_image_offset[\s\S]*?buffer_image_extent[\s\S]*?buffer_image_row_length[\s\S]*?buffer_image_height[\s\S]*?buffer_image_bounds",
            "buffer-image fallback telemetry does not identify the actual unsupported shape",
        ),
        (
            transfer_mm + queue_mm + e2e,
            r"VK_REMAINING_ARRAY_LAYERS[\s\S]*?MTLTextureType3D[\s\S]*?copyBufferImage[\s\S]*?bytesPerImage[\s\S]*?MTLBlitOptionDepthFromDepthStencil[\s\S]*?MTLBlitOptionStencilFromDepthStencil[\s\S]*?options:[\s\S]*?BUFFER_IMAGE_LAYERED_OK[\s\S]*?BUFFER_IMAGE_3D_OK[\s\S]*?BUFFER_IMAGE_DEPTH_STENCIL_OK",
            "layered, 3D, and depth/stencil buffer-image copies are not materialized and read back through Metal 4",
        ),
        (
            queries_h + queries_mm + queue_mm + query_pool_h + e2e,
            r"MVKCmdCopyQueryPoolResults[\s\S]*?prepareMetal4Encoding[\s\S]*?copyQueryPoolResults[\s\S]*?getMetal4ResultMTLBuffer[\s\S]*?QUERY_COPY_RESULTS_OK",
            "packed WAIT query results are not copied through the Metal 4 path",
        ),
        (
            rendering_mm + queue_mm + transfer_mm + e2e,
            r"framebufferLayerCount[\s\S]*?MTLTextureType2DArray[\s\S]*?texture\.arrayLength[\s\S]*?renderTargetArrayLength\s*=\s*framebuffer->getLayerCount\(\)[\s\S]*?CLASSIC_LAYERED_RENDER_OK",
            "classic layered render passes are not admitted and exercised through the Metal 4 path",
        ),
        (
            command_h + command_buffer_h + command_buffer_mm + transfer_mm + queue_mm + command_resource_factory_mm + e2e,
            r"framebufferLayerCount[\s\S]*?beginRenderpass[\s\S]*?framebuffer[\s\S]*?enableLayeredRendering[\s\S]*?rect\.layerCount[\s\S]*?render_target_array_index[\s\S]*?CLASSIC_LAYERED_CLEAR_ATTACHMENTS_OK",
            "classic layered vkCmdClearAttachments is not retained, materialized, and read back through Metal 4",
        ),
    ):
        require(source, pattern, message)
    reject(
        pipeline_mm,
        r"attachment_render_pass_simple",
        "supported single-color classic render passes are still mislabeled as attachment blockers",
    )
    reject(
        rendering_mm,
        r"_supportsMetal4Encoding\s*=\s*subpass[\s\S]*?!subpass->isStencilAttachmentUsed\(\)",
        "classic render commands still reject an inert stencil attachment before pipeline eligibility can fail closed",
    )
    reject(
        pipeline_mm,
        r'"MVKCmdBindGraphicsPipeline:dynamic_state"',
        "dynamic-state blockers are still collapsed into one unactionable bucket",
    )
    reject(
        execute_metal4,
        r"\[options release\];\s*\[options release\];",
        "a Metal 4 exception path releases commit options twice",
    )

    # Hybrid backends are totally ordered, including fallback, present, and wait-idle.
    for token in (
        "reserveMetal4SubmissionSequence",
        "encodeMetal4OrderingWait",
        "encodeMetal4OrderingSignal",
        "waitForEvent",
        "signalEvent",
        "MVKQueuePresentSurfaceSubmission::execute",
        "MVKQueue::waitIdle",
    ):
        require(queue_mm, re.escape(token), f"hybrid queue ordering is missing: {token}")
    require(
        queue_mm,
        r"isPrefilled[\s\S]*?empty bridge[\s\S]*?encodeMetal4OrderingWait",
        "prefilled legacy command buffers can run before the hybrid ordering wait",
    )

    # Native binary/timeline events, emulated waits, and the one-Vulkan-queue style
    # are mapped. The latter relies on the hybrid ordering event that bridges the
    # independent legacy and Metal 4 queues.
    require(sync_h, r"supportsMetal4QueueEncoding\(\)\s*\{\s*return\s+false", "base semaphore support does not fail closed")
    require(sync_mm, r"MVKSemaphoreMTLEvent::encodeMetal4Wait[\s\S]*?waitForEvent", "binary Metal-event wait is missing")
    require(sync_mm, r"MVKSemaphoreMTLEvent::encodeMetal4Signal[\s\S]*?signalEvent", "binary Metal-event signal is missing")
    require(sync_mm, r"MVKTimelineSemaphoreMTLEvent::encodeMetal4Wait[\s\S]*?waitForEvent", "timeline wait is missing")
    require(sync_mm, r"MVKTimelineSemaphoreMTLEvent::encodeMetal4Signal[\s\S]*?signalEvent", "timeline signal is missing")
    require(sync_mm, r"MVKSemaphoreEmulated::encodeMetal4Wait[\s\S]*?encodeWait\(nil", "emulated semaphore wait is missing")
    require(
        function_body(sync_h, "class MVKSemaphoreSingleQueue", "#pragma mark -\n#pragma mark MVKSemaphoreMTLEvent"),
        r"supportsMetal4QueueEncoding\(\)\s*override\s*\{\s*return\s+true",
        "single-queue semaphores still reject the hybrid-ordered Metal 4 path",
    )

    # Independent Vulkan e2e validates hybrid order, binary/timeline semaphores,
    # descriptorless compute, image data, a real dynamic-rendering draw, barriers,
    # pixel readback, and exact path telemetry.
    for token in (
        "vkCmdFillBuffer",
        "vkCmdUpdateBuffer",
        "vkCmdResetQueryPool",
        "vkCmdBeginQuery",
        "vkCmdEndQuery",
        "vkCmdPipelineBarrier",
        "vkCmdCopyBuffer",
        "vkCmdCopyImage",
        "vkCmdDispatch",
        "vkCreateComputePipelines",
        "vkCreateGraphicsPipelines",
        "vkCmdBeginRendering",
        "vkCmdDraw",
        "vkCmdEndRendering",
        "vkCreateSemaphore",
        "VK_SEMAPHORE_TYPE_TIMELINE",
        "vkWaitForFences",
        "vkInvalidateMappedMemoryRanges",
        "TIMELINE_OK",
        "COMPUTE_OK",
        "IMAGE_DATA_OK",
        "RENDER_OK",
        "QUERY_RESET_OK",
        "QUERY_OCCLUSION_OK",
        "COMPUTE_REBIND_AFTER_RENDER_OK",
        "QUERY_OUTSIDE_RENDER_SCOPE_OK",
        "GRAPHICS_REBIND_AFTER_RENDER_OK",
        "UPDATE_BUFFER_OK",
        "METAL4_PHASE1C_E2E_PASS",
    ):
        require(e2e, re.escape(token), f"Vulkan e2e coverage is missing: {token}")
    require(runner, r"MVK_CONFIG_METAL4_COMMAND_BACKEND=0", "legacy control run is missing")
    require(runner, r"MVK_CONFIG_METAL4_COMMAND_BACKEND=1", "Metal 4 run is missing")
    require(
        runner,
        r"MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=0[\s\S]*?MVK_CONFIG_METAL4_COMMAND_BACKEND=1",
        "Metal 4 E2E does not exercise MeloNX's single-queue semaphore style",
    )
    require(runner, r"Executed first Vulkan submission on the Metal 4 transfer backend", "runtime path proof is missing")
    for counter in (
        "image_copies",
        "compute_dispatches",
        "render_submissions",
        "render_passes",
        "draws",
        "barriers",
        "buffer_updates",
    ):
        require(runner, rf"{counter}=\[1-9\]", f"strict runtime counter gate is missing: {counter}")
    require(runner, r"fallbacks=0", "controlled Metal 4 E2E does not require a zero-fallback path")
    require(runner, r"unsupported_commands=none", "controlled Metal 4 E2E permits unsupported commands")

    print("PASS: usable Metal 4 Phase 1C compute/transfer/render backend source contract")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
