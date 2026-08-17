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
    pipeline_cmd_h = read("MoltenVK/MoltenVK/Commands/MVKCmdPipeline.h")
    pipeline_cmd_mm = read("MoltenVK/MoltenVK/Commands/MVKCmdPipeline.mm")
    pipeline_h = read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h")
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
         pipeline_cmd_h, pipeline_cmd_mm, pipeline_h, queue_h, queue_mm, sync_h, sync_mm)
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
        r"supportsMetal4Semaphores\(\)[\s\S]*?supportsMetal4CommandBuffers\(\)[\s\S]*?prepareMetal4CommandBuffers",
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
    for command in ("MVKCmdCopyBuffer", "MVKCmdFillBuffer"):
        require(transfer_h, rf"{command}[\s\S]*?supportsMetal4Encoding", f"{command} is not opted in")
        require(transfer_h, rf"{command}[\s\S]*?prepareMetal4Encoding", f"{command} does not resolve resources")
        require(transfer_h, rf"{command}[\s\S]*?encodeMetal4", f"{command} does not materialize")
    require(
        transfer_mm,
        r"supportsMetal4CopyBuffer[\s\S]*?regions\.empty\(\)[\s\S]*?mtlCopyBufferAlignment[\s\S]*?srcOffset\s*%\s*alignment[\s\S]*?dstOffset\s*%\s*alignment[\s\S]*?size\s*%\s*alignment",
        "copy eligibility does not reject empty work or preserve Metal alignment rules",
    )
    require(transfer_mm, r"0x01010101u", "fill eligibility does not require a repeated byte")
    require(queue_mm, r"MTL4ComputeCommandEncoder", "real MTL4 compute/transfer encoder is missing")
    require(queue_mm, r"copyFromBuffer:[\s\S]*?destinationOffset:[\s\S]*?size:", "MTL4 buffer copy is missing")
    require(queue_mm, r"fillBuffer:[\s\S]*?range:[\s\S]*?value:", "MTL4 buffer fill is missing")
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

    for token in (
        "supportsMetal4DescriptorlessExecution",
        "MVKCmdBindComputePipeline",
        "useComputePipeline",
        "bindComputePipeline",
        "MVKCmdDispatch",
        "dispatchThreadgroups",
        "setComputePipelineState",
    ):
        require(implementation, re.escape(token), f"descriptorless compute path is missing: {token}")
    require(
        pipeline_h,
        r"supportsMetal4DescriptorlessExecution[\s\S]*?resources\.allBits\.empty[\s\S]*?implicitBuffers\.needed\.empty",
        "compute pipeline eligibility does not reject descriptor or implicit-buffer use",
    )
    require(
        dispatch_h + dispatch_mm,
        r"_baseGroupX\s*==\s*0[\s\S]*?_baseGroupY\s*==\s*0[\s\S]*?_baseGroupZ\s*==\s*0",
        "MTL4 dispatch accepts unsupported non-zero dispatch bases",
    )

    require(
        pipeline_cmd_h + pipeline_cmd_mm,
        r"MVKCmdPipelineBarrier[\s\S]*?supportsMetal4PipelineBarriers[\s\S]*?prepareMetal4Encoding[\s\S]*?encodeMetal4",
        "buffer/memory pipeline barriers are not materialized",
    )
    require(
        queue_mm,
        r"PendingBarrier[\s\S]*?barrierAfterQueueStages:[\s\S]*?beforeStages:[\s\S]*?visibilityOptions:",
        "cross-encoder MTL4 consumer barrier is missing",
    )
    reject(
        queue_mm,
        r"_computeEncoder\s+barrierAfterEncoderStages:[\s\S]*?MTLStageVertex|_renderEncoder\s+barrierAfterEncoderStages:[\s\S]*?MTLStageBlit",
        "render/compute stage masks are passed to an incompatible intra-pass encoder barrier",
    )
    require(
        pipeline_cmd_mm,
        r"_dependencyFlags\s*==\s*0[\s\S]*?supportsMetal4PipelineBarriers",
        "unsupported dependency flags do not fail closed",
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

    # Strict ordinary-render slice: one dynamic-rendering color attachment, a descriptorless
    # graphics pipeline, and a real non-indexed draw on MTL4RenderCommandEncoder.
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
        r"mvkSupportsMetal4RenderingInfo[\s\S]*?colorAttachmentCount\s*!=\s*1[\s\S]*?pDepthAttachment[\s\S]*?pStencilAttachment[\s\S]*?VK_SAMPLE_COUNT_1_BIT",
        "dynamic-rendering eligibility does not fail closed on unsupported attachments or multisampling",
    )
    require(
        pipeline_cmd_h + pipeline_cmd_mm,
        r"MVKCmdBindGraphicsPipeline[\s\S]*?supportsMetal4DescriptorlessRenderExecution[\s\S]*?useGraphicsPipeline[\s\S]*?bindGraphicsPipeline",
        "descriptorless graphics-pipeline binding is not materialized",
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
        draw_h + draw_mm,
        r"MVKCmdDraw[\s\S]*?supportsMetal4Encoding[\s\S]*?prepareMetal4Encoding[\s\S]*?encodeMetal4[\s\S]*?cmdEncoder->draw",
        "real non-indexed Vulkan draw is not routed to Metal 4",
    )
    for token in (
        "MTL4RenderPassDescriptor",
        "renderCommandEncoderWithDescriptor",
        "setRenderPipelineState",
        "setViewport",
        "setScissorRect",
        "drawPrimitives",
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
    require(
        execute_metal4,
        r"commitAttempted\s*=\s*true[\s\S]*?commit:[\s\S]*?@catch[\s\S]*?if\s*\(!commitAttempted\)[\s\S]*?endMetal4CommandBuffers\(false\)",
        "pre-commit exceptions are not separated from ambiguous post-commit failures",
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

    # Native binary/timeline events and emulated waits are mapped; implicit single-queue semaphores fail closed.
    require(sync_h, r"supportsMetal4QueueEncoding\(\)\s*\{\s*return\s+false", "base semaphore support does not fail closed")
    require(sync_mm, r"MVKSemaphoreMTLEvent::encodeMetal4Wait[\s\S]*?waitForEvent", "binary Metal-event wait is missing")
    require(sync_mm, r"MVKSemaphoreMTLEvent::encodeMetal4Signal[\s\S]*?signalEvent", "binary Metal-event signal is missing")
    require(sync_mm, r"MVKTimelineSemaphoreMTLEvent::encodeMetal4Wait[\s\S]*?waitForEvent", "timeline wait is missing")
    require(sync_mm, r"MVKTimelineSemaphoreMTLEvent::encodeMetal4Signal[\s\S]*?signalEvent", "timeline signal is missing")
    require(sync_mm, r"MVKSemaphoreEmulated::encodeMetal4Wait[\s\S]*?encodeWait\(nil", "emulated semaphore wait is missing")
    reject(
        function_body(sync_h, "class MVKSemaphoreSingleQueue", "#pragma mark -\n#pragma mark MVKSemaphoreMTLEvent"),
        r"supportsMetal4QueueEncoding\(\)\s*override\s*\{\s*return\s+true",
        "implicit single-queue semaphore was incorrectly accepted across two Metal queues",
    )

    # Independent Vulkan e2e validates hybrid order, binary/timeline semaphores,
    # descriptorless compute, image data, a real dynamic-rendering draw, barriers,
    # pixel readback, and exact path telemetry.
    for token in (
        "vkCmdFillBuffer",
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
        "METAL4_PHASE1C_E2E_PASS",
    ):
        require(e2e, re.escape(token), f"Vulkan e2e coverage is missing: {token}")
    require(runner, r"MVK_CONFIG_METAL4_COMMAND_BACKEND=0", "legacy control run is missing")
    require(runner, r"MVK_CONFIG_METAL4_COMMAND_BACKEND=1", "Metal 4 run is missing")
    require(runner, r"Executed first Vulkan submission on the Metal 4 transfer backend", "runtime path proof is missing")
    for counter in (
        "image_copies",
        "compute_dispatches",
        "render_submissions",
        "render_passes",
        "draws",
        "barriers",
    ):
        require(runner, rf"{counter}=\[1-9\]", f"strict runtime counter gate is missing: {counter}")

    print("PASS: usable Metal 4 Phase 1C compute/transfer/render backend source contract")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
