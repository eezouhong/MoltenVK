#!/usr/bin/env python3
"""Lock the safe Phase 1 Metal 4 command-backend boundary.

Phase 1A validates queue/allocator/command-buffer object creation. Phase 1B adds
one bounded internal empty commit and allocator-lifetime tracking. Vulkan
submissions must remain on the legacy Metal path until synchronization,
residency, command encoding, and completion ownership are implemented together.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative_path: str) -> str:
    path = ROOT / relative_path
    if not path.is_file():
        raise AssertionError(f"missing source file: {relative_path}")
    return path.read_text(encoding="utf-8")


def require(source: str, pattern: str, message: str) -> None:
    if not re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


def reject(source: str, pattern: str, message: str) -> None:
    if re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


def function_body(source: str, signature: str, next_signature: str) -> str:
    start = source.find(signature)
    if start < 0:
        raise AssertionError(f"missing function: {signature}")
    end = source.find(next_signature, start + len(signature))
    if end < 0:
        raise AssertionError(
            f"missing boundary after {signature}: {next_signature}"
        )
    return source[start:end]


def main() -> int:
    queue_h = read("MoltenVK/MoltenVK/GPUObjects/MVKQueue.h")
    queue_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm")
    config_members = read("MoltenVK/MoltenVK/Utility/MVKConfigMembers.def")
    implementation = queue_h + "\n" + queue_mm

    # Keep the experiment private and disabled unless explicitly requested.
    require(
        queue_mm,
        r'mvkGetEnvVarNumber\(\s*"MVK_CONFIG_METAL4_COMMAND_BACKEND"\s*,\s*0\.0\s*\)',
        "Metal 4 command backend gate is missing or not default-off",
    )
    reject(
        config_members,
        r"METAL4_COMMAND_BACKEND|METAL4_COMMAND_ALLOCATOR_COUNT",
        "Phase 1 gates must not change the public MVKConfiguration ABI",
    )

    # Preserve the same platform and compatibility boundary as the compiler POC.
    require(
        implementation,
        r"MVK_XCODE_26\s*&&\s*!MVK_TVOS\s*&&\s*!MVK_VISIONOS\s*&&\s*!MVK_OS_SIMULATOR",
        "Xcode 26/unsupported-target compile guard is missing",
    )
    require(
        queue_mm,
        r"#if\s+MVK_USE_METAL_PRIVATE_API",
        "compile-time private-API exclusion is missing",
    )
    require(
        queue_mm,
        r"getMVKConfig\(\)\.useMetalPrivateAPI",
        "runtime private-API exclusion is missing",
    )
    require(queue_mm, r"supportsMetal4", "Metal 4 GPU capability gate is missing")
    require(
        queue_mm,
        r"mvkOSVersionIsAtLeast\(\s*26\.0\s*\)",
        "OS 26 runtime gate is missing",
    )
    require(
        queue_mm,
        r"_device->hasResidencySet\(\)",
        "actual committed primary residency-set gate is missing",
    )

    # Prove all three foundational Metal 4 command objects exist and can encode.
    for selector in (
        "newMTL4CommandQueue",
        "newCommandAllocator",
        "newCommandBuffer",
    ):
        require(
            queue_mm,
            rf"@selector\({selector}\)",
            f"runtime selector check is missing: {selector}",
        )
    require(
        queue_h,
        r"id<MTL4CommandQueue>\s+_mtl4Queue",
        "queue-owned MTL4CommandQueue is missing",
    )
    require(
        queue_mm,
        r"id<MTL4CommandAllocator>\s+allocator\s*=\s*\[mtlDevice\s+newCommandAllocator\]",
        "MTL4CommandAllocator creation is missing",
    )
    require(
        queue_mm,
        r"id<MTL4CommandBuffer>\s+commandBuffer\s*=\s*\[[^\]]+\s+newCommandBuffer\]",
        "MTL4CommandBuffer creation is missing",
    )
    require(
        queue_mm,
        r"beginCommandBufferWithAllocator\s*:\s*allocator",
        "MTL4CommandBuffer begin/allocator handoff is missing",
    )
    require(
        queue_mm,
        r"\[commandBuffer\s+endCommandBuffer\]",
        "MTL4CommandBuffer finalization is missing",
    )
    require(queue_mm, r"\[_mtl4Queue\s+release\]", "MTL4 queue teardown is missing")

    # Bound the allocator arena and separate encoding from GPU completion.
    require(
        queue_mm,
        r"kMetal4CommandAllocatorDefaultCount\s*=\s*4",
        "bounded allocator default is missing",
    )
    require(
        queue_mm,
        r"kMetal4CommandAllocatorMaxCount\s*=\s*16",
        "allocator hard limit is missing",
    )
    require(
        queue_mm,
        r'mvkGetEnvVarNumber\(\s*"MVK_CONFIG_METAL4_COMMAND_ALLOCATOR_COUNT"',
        "private allocator-count gate is missing",
    )
    for token in (
        "MVKMetal4CommandQueueState",
        "AllocatorSlot",
        "inFlightCount",
        "resetPending",
        "acquireAllocator",
        "finishEncoding",
        "completeSubmission",
        "cancelSubmittedEncoding",
    ):
        require(implementation, re.escape(token), f"allocator ownership state is missing: {token}")
    require(
        queue_h,
        r"std::shared_ptr<MVKMetal4CommandQueueState>\s+_metal4CommandState",
        "queue does not own the Metal 4 state through shared lifetime",
    )
    require(
        queue_mm,
        r"if\s*\(slot\.inFlightCount\s*==\s*0\)[\s\S]*?\[slot\.allocator\s+reset\]",
        "allocator reset is not gated on zero in-flight command buffers",
    )
    reject(
        function_body(
            queue_mm,
            "void MVKMetal4CommandQueueState::shutdown()",
            "#endif",
        ) if "void MVKMetal4CommandQueueState::shutdown()" in queue_mm else "",
        r"wait|join",
        "queue teardown must not block indefinitely for late feedback",
    )

    # Phase 1B may submit only its internal empty probe. The feedback callback
    # must retain shared state and must never dereference queue/device objects.
    probe = function_body(
        queue_mm,
        "bool MVKQueue::startMTL4CommandSubmissionProbe()",
        "// Creates the independent Metal 4 queue",
    )
    require(
        probe,
        r"auto\s+state\s*=\s*_metal4CommandState",
        "probe callback does not capture self-contained shared state",
    )
    require(
        probe,
        r"MTL4CommitOptions\s*\*\s*options\s*=\s*\[MTL4CommitOptions\s+new\]",
        "commit options and feedback ownership are missing",
    )
    require(
        probe,
        r"addFeedbackHandler\s*:\s*\^\s*\(id<MTL4CommitFeedback>\s+feedback\)",
        "Metal 4 completion feedback handler is missing",
    )
    feedback_match = re.search(
        r"addFeedbackHandler\s*:\s*\^\s*\(id<MTL4CommitFeedback>\s+feedback\)\s*\{(?P<body>.*?)\}\s*\]",
        probe,
        re.MULTILINE | re.DOTALL,
    )
    if not feedback_match:
        raise AssertionError("unable to isolate Metal 4 feedback callback")
    feedback_body = feedback_match.group("body")
    require(
        feedback_body,
        r"state->completeSubmission\(slotIndex,\s*feedback\.error\)",
        "feedback callback does not complete the allocator slot",
    )
    reject(
        feedback_body,
        r"\bthis\b|_queue|_device|_mtl4Queue|getDevice",
        "late feedback callback captures queue/device lifetime",
    )
    require(
        probe,
        r"\[_mtl4Queue\s+commit\s*:\s*commandBuffers\s+count\s*:\s*1\s+options\s*:\s*options\]",
        "bounded internal Metal 4 commit is missing",
    )
    if queue_mm.count("[_mtl4Queue commit:") != 1:
        raise AssertionError("only the bounded internal probe may commit to MTL4 in Phase 1B")
    require(
        queue_mm,
        r"_metal4CommandState->shutdown\(\)[\s\S]*?\[_mtl4Queue\s+release\]",
        "teardown does not close state before releasing the queue",
    )

    # Expose readiness without changing the Vulkan API or selecting per command.
    for token in (
        "wasMetal4CommandBackendRequested",
        "isMetal4CommandBackendReady",
        "isMetal4CommandSubmissionReady",
        "getMTL4CommandQueue",
        "initMTL4CommandQueue",
        "validateMTL4CommandObjects",
        "startMTL4CommandSubmissionProbe",
    ):
        require(implementation, re.escape(token), f"missing Phase 1 boundary: {token}")

    # No Vulkan submit/present work may reach MTL4 until real command ownership,
    # barriers, descriptor tables, and residency are complete.
    submit_execute = function_body(
        queue_mm,
        "VkResult MVKQueueCommandBufferSubmission::execute()",
        "// Returns the active MTLCommandBuffer",
    )
    reject(
        submit_execute,
        r"MTL4|_mtl4Queue|isMetal4CommandSubmissionReady",
        "Vulkan command submission was moved to Metal 4 prematurely",
    )
    present_execute = function_body(
        queue_mm,
        "VkResult MVKQueuePresentSurfaceSubmission::execute()",
        "void MVKQueuePresentSurfaceSubmission::finish()",
    )
    reject(
        present_execute,
        r"MTL4|_mtl4Queue|isMetal4CommandSubmissionReady",
        "Vulkan presentation was moved to Metal 4 prematurely",
    )
    require(
        queue_h,
        r"id<MTLCommandBuffer>\s+_activeMTLCommandBuffer",
        "legacy submission ownership was unexpectedly removed",
    )
    require(queue_mm, r"\[mtlCmdBuff\s+commit\]", "legacy commit path was removed")
    require(
        queue_mm,
        r"Phase 1 keeps all Vulkan submissions on the legacy Metal queue",
        "explicit safe-fallback diagnostic is missing",
    )

    # Render/compute/argument-table migration belongs to the next checkpoint.
    reject(
        implementation,
        r"MTL4(?:Render|Compute)CommandEncoder|MTL4ArgumentTable",
        "Phase 1B must not partially introduce encoder or argument-table execution",
    )

    print("PASS: Metal 4 Phase 1A/1B object, allocator, and empty-commit boundary is safe")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
