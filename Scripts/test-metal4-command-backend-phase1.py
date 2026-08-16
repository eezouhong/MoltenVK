#!/usr/bin/env python3
"""Lock the safe Phase 1 Metal 4 command-backend boundary.

This source contract deliberately verifies only queue/allocator/command-buffer
object readiness. Vulkan submissions must remain on the legacy Metal command
path until the Phase 1 submission context, synchronization, residency, and
completion ownership are implemented together.
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
        r"METAL4_COMMAND_BACKEND",
        "Phase 1 gate must not change the public MVKConfiguration ABI",
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
    require(
        queue_mm,
        r"supportsMetal4",
        "Metal 4 GPU capability gate is missing",
    )
    require(
        queue_mm,
        r"mvkOSVersionIsAtLeast\(\s*26\.0\s*\)",
        "OS 26 runtime gate is missing",
    )
    require(
        queue_mm,
        r"getMetalFeatures\(\)\.residencySets",
        "residency-set capability gate is missing",
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
        r"id<MTL4CommandBuffer>\s+commandBuffer\s*=\s*\[mtlDevice\s+newCommandBuffer\]",
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
    require(
        queue_mm,
        r"\[_mtl4Queue\s+release\]",
        "MTL4CommandQueue teardown is missing",
    )

    # Expose readiness without changing the Vulkan API or selecting per command.
    for token in (
        "wasMetal4CommandBackendRequested",
        "isMetal4CommandBackendReady",
        "getMTL4CommandQueue",
        "initMTL4CommandQueue",
        "validateMTL4CommandObjects",
    ):
        require(queue_h + queue_mm, re.escape(token), f"missing Phase 1 boundary: {token}")

    # The safety invariant for this checkpoint: no Vulkan submit/present work is
    # permitted to reach MTL4 until a complete submission context is present.
    submit_execute = function_body(
        queue_mm,
        "VkResult MVKQueueCommandBufferSubmission::execute()",
        "// Returns the active MTLCommandBuffer",
    )
    reject(
        submit_execute,
        r"MTL4|_mtl4Queue",
        "Vulkan command submission was moved to Metal 4 before Phase 1 ownership is complete",
    )
    present_execute = function_body(
        queue_mm,
        "VkResult MVKQueuePresentSurfaceSubmission::execute()",
        "void MVKQueuePresentSurfaceSubmission::finish()",
    )
    reject(
        present_execute,
        r"MTL4|_mtl4Queue",
        "Vulkan presentation was moved to Metal 4 before drawable synchronization is complete",
    )
    require(
        queue_h,
        r"id<MTLCommandBuffer>\s+_activeMTLCommandBuffer",
        "legacy submission ownership was unexpectedly removed",
    )
    require(
        queue_mm,
        r"\[mtlCmdBuff\s+commit\]",
        "legacy commit path was unexpectedly removed",
    )
    require(
        queue_mm,
        r"Phase 1 keeps all Vulkan submissions on the legacy Metal queue",
        "explicit safe-fallback diagnostic is missing",
    )

    # Render/compute/argument-table migration belongs to later checkpoints.
    reject(
        implementation,
        r"MTL4(?:Render|Compute)CommandEncoder|MTL4ArgumentTable",
        "Phase 1A must not partially introduce encoder or argument-table execution",
    )

    print("PASS: Metal 4 Phase 1A command object boundary is safe and default-off")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
