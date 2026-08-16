#!/usr/bin/env python3
"""Lock the default-off Metal 4 command transport and its fallback boundary."""

from pathlib import Path
import re


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


def main() -> int:
    queue_h = read("MoltenVK/MoltenVK/GPUObjects/MVKQueue.h")
    queue_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm")
    config_members = read("MoltenVK/MoltenVK/Utility/MVKConfigMembers.def")
    implementation = queue_h + "\n" + queue_mm

    require(
        queue_mm,
        r'mvkGetEnvVarNumber\(\s*"MVK_CONFIG_METAL4_COMMAND_BACKEND",\s*0\.0\s*\)',
        "command backend gate is missing or is not default-off",
    )
    reject(
        config_members,
        r"METAL4_COMMAND",
        "experimental command transport must not change the public MVKConfiguration ABI",
    )

    for token in (
        "class MVKMetal4CommandTransport",
        "MVKMetal4CommandTransport* _metal4CommandTransport",
        "isMetal4CommandTransportEnabled",
        "initMetal4CommandTransport",
        "destroyMetal4CommandTransport",
        "MVK_CONFIG_METAL4_COMMAND_ALLOCATOR_MAX",
        "MVK_CONFIG_METAL4_COMMAND_VALIDATION_TIMEOUT_MS",
        "newMTL4CommandQueueWithDescriptor",
        "newCommandAllocatorWithDescriptor",
        "newCommandBuffer",
        "beginCommandBufferWithAllocator",
        "endCommandBuffer",
        "MTL4CommitOptions",
        "addFeedbackHandler",
        "commit:commandBuffers count:1 options:options",
    ):
        require(implementation, re.escape(token), f"missing command transport token: {token}")

    require(
        implementation,
        r"#if\s+MVK_XCODE_26\s+&&\s+!MVK_TVOS\s+&&\s+!MVK_VISIONOS\s+&&\s+!MVK_OS_SIMULATOR",
        "Metal 4 command code is not isolated behind the target guard",
    )
    require(implementation, r"#if\s+MVK_USE_METAL_PRIVATE_API", "compile-time private API gate is missing")
    require(
        queue_mm,
        r"getMVKConfig\(\)\.useMetalPrivateAPI",
        "runtime private API gate is missing",
    )
    require(queue_mm, r"supportsMetal4", "Metal 4 GPU capability gate is missing")
    require(
        queue_mm,
        r"mvkOSVersionIsAtLeast\s*\(\s*26\.0\s*\)",
        "OS 26 availability gate is missing",
    )

    require(queue_mm, r"vector<AllocatorSlot>", "bounded allocator arena is missing")
    require(queue_mm, r"_allocatorReady\.wait_for", "allocator leasing is not bounded")
    require(queue_mm, r"_shuttingDown\s*=\s*true", "allocator teardown does not wake or reject waiters")
    require(queue_mm, r"_allocatorReady\.notify_all", "allocator teardown wake-up is missing")
    require(
        queue_mm,
        r"requestedAllocatorCount[\s\S]*?1\.0[\s\S]*?kMetal4CommandAllocatorMaximumCount",
        "allocator count is not clamped",
    )

    require(queue_mm, r"ready\.wait_for", "commit feedback wait is not bounded")
    require(
        queue_mm,
        r"make_shared<MVKMetal4CommandFeedbackContext>",
        "commit feedback context is not heap-owned",
    )
    require(
        queue_mm,
        r"queue\(\[mtlQueue retain\]\)[\s\S]*?commandBuffer\(\[mtlCommandBuffer retain\]\)[\s\S]*?allocator\(\[mtlAllocator retain\]\)",
        "late feedback context does not retain all Metal inputs",
    )
    require(
        queue_mm,
        r"addFeedbackHandler:\^\(id<MTL4CommitFeedback> feedback\)\s*\{\s*context->complete\(feedback\);\s*\}",
        "feedback block must capture only the heap context",
    )
    reject(
        queue_mm,
        r"addFeedbackHandler:[\s\S]{0,300}\bthis\b",
        "feedback block captures a transport or queue pointer",
    )

    require(
        queue_mm,
        r"_mtlQueue\s*=\s*_queueFamily->getMTLCommandQueue",
        "legacy Metal queue creation was removed",
    )
    require(
        queue_mm,
        r"Vulkan command encoding remains on the legacy backend",
        "phase boundary is not explicit in runtime diagnostics",
    )
    reject(
        queue_mm,
        r"MVKQueueCommandBufferSubmission::execute\([\s\S]*?_metal4CommandTransport",
        "Phase 1 transport must not redirect Vulkan submissions before encoder coverage",
    )

    print("PASS: Metal 4 command transport is gated, bounded, teardown-safe, and legacy-authoritative")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
