#!/usr/bin/env python3
"""Verify the managed-to-native Metal 4 priority scope is wired end to end."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(source: str, pattern: str, message: str) -> None:
    if not re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


def main() -> int:
    private_api = read("MoltenVK/MoltenVK/API/mvk_private_api.h")
    api = read("MoltenVK/MoltenVK/Vulkan/mvk_api.mm")
    pipeline_h = read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h")
    pipeline = read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm")

    for token in (
        "MVKMetal4CompilerWorkUrgency",
        "MVK_METAL4_COMPILER_WORK_URGENCY_LIFECYCLE",
        "MVK_METAL4_COMPILER_WORK_URGENCY_DEMANDED",
        "MVK_METAL4_COMPILER_WORK_URGENCY_BLOCKING",
        "PFN_vkBeginMetal4CompilerWorkScopeMVK",
        "PFN_vkPromoteMetal4CompilerWorkScopeMVK",
        "PFN_vkEndMetal4CompilerWorkScopeMVK",
        "vkBeginMetal4CompilerWorkScopeMVK",
        "vkPromoteMetal4CompilerWorkScopeMVK",
        "vkEndMetal4CompilerWorkScopeMVK",
    ):
        require(private_api, re.escape(token), f"missing priority-scope ABI: {token}")

    require(
        api,
        r"vkBeginMetal4CompilerWorkScopeMVK.*?beginPriorityWork.*?"
        r"vkPromoteMetal4CompilerWorkScopeMVK.*?promotePriorityWork.*?"
        r"vkEndMetal4CompilerWorkScopeMVK.*?endPriorityWork.*?mvkCopyGrowingStruct",
        "priority-scope ABI is not wired to the compiler service",
    )
    require(
        pipeline_h,
        r"beginPriorityWork.*?promotePriorityWork.*?endPriorityWork",
        "compiler service priority-scope methods are missing",
    )
    require(
        pipeline,
        r"MVKMetal4CompilerAdmission\.h.*?Metal4AdmissionQueue\s+compilerAdmissionQueue",
        "the real compiler service does not own the ordered admission queue",
    )
    require(
        pipeline,
        r"acquireMetal4CompilerSlot.*?compilerAdmissionQueue\.enqueue.*?"
        r"compilerAdmissionQueue\.canAdmit.*?compilerAdmissionQueue\.erase",
        "the production compiler slot gate does not use ordered admission",
    )
    require(
        pipeline,
        r"promotePriorityWork.*?promoteMetal4AdmissionScope.*?"
        r"compilerSlotReady\.notify_all",
        "live promotion does not wake and reorder native waiters",
    )
    require(
        pipeline,
        r"struct\s+BaseEntry.*?admissionScope.*?entry->admissionScope.*?"
        r"promoteMetal4AdmissionScope",
        "a blocking waiter cannot promote the owner of a coalesced Metal 4 base",
    )
    require(
        pipeline,
        r"finishMetal4CompilerSlot.*?compilerTasksInFlight--.*?"
        r"compilerSlotReady\.notify_all",
        "slot completion must continue waking the shared ordered gate",
    )
    print("Metal 4 priority scope contract passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
