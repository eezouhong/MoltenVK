#!/usr/bin/env python3
"""Source and policy-contract checks for the fixed-1024 Metal 4 base cache."""

from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
PIPELINE = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm"
U64_MAX = (1 << 64) - 1
UNKNOWN_SIZE_FLOOR = 64 * 1024


def require(text: str, needle: str, message: str) -> None:
    if needle not in text:
        raise AssertionError(message)


def saturating_add(left: int, right: int) -> int:
    return min(U64_MAX, left + right)


def saturating_multiply(left: int, right: int) -> int:
    return min(U64_MAX, left * right)


def score(cost_ns: int, frequency: int, allocated_bytes: int, aging: int = 0) -> int:
    cost_us = max(1, cost_ns // 1000)
    bounded_frequency = min(1024, max(1, frequency))
    size_kib = max(1, max(allocated_bytes, UNKNOWN_SIZE_FLOOR) // 1024)
    benefit = max(1, saturating_multiply(cost_us, bounded_frequency) // size_kib)
    return saturating_add(aging, benefit)


def main() -> int:
    try:
        source = PIPELINE.read_text(encoding="utf-8")
        required = (
            "MVK_CONFIG_METAL4_FLEXIBLE_CACHE_POLICY",
            "MVKMetal4BaseGhostEntry",
            "array<MVKMetal4BaseGhostEntry, 512>",
            "kMetal4CacheConfiguredHardLimit = 1024",
            "saturatingMetal4Add",
            "saturatingMetal4Multiply",
            "scoreMetal4BaseCacheEntry",
            "kMetal4UnknownAllocationSizeFloor",
            "numeric_limits<uint64_t>::max()",
            "basePipeline.allocatedSize",
            "os_proc_available_memory()",
            "findMetal4BaseCacheVictim",
            "entry->score > victim->second->score",
            "cachePolicy == 0",
            "kMetal4PendingCandidateLimit = 16",
            "impl->dynamicCacheTarget = impl->cacheMax",
            "condition_variable baseCandidateReady",
            "impl->baseCandidateReady.wait",
            "impl->pendingCompilations < kMetal4PendingCandidateLimit",
            "pendingCapacityWaits",
            "pendingCapacityWaitTotalNs",
            "pendingCapacityWaitMaxNs",
            "recordMetal4BaseGhost",
            "candidateRejections",
            "pressureEvictions",
            "residentMeasuredBytes",
        )
        for token in required:
            require(source, token, f"missing production policy token: {token}")

        if not re.search(
            r"mvkClamp\(\s*configuredMax,\s*1\.0,\s*"
            r"(?:kMetal4CacheConfiguredHardLimit|1024\.0)\s*\)",
            source,
        ):
            raise AssertionError("cache maximum is not clamped to 1024")
        if "unordered_map<uint64_t, MVKMetal4BaseGhostEntry>" in source:
            raise AssertionError("ghost history is unbounded")
        if "pendingLimitFallbacks" in source:
            raise AssertionError("pending-base saturation must wait instead of bypassing Metal 4")
        for forbidden in (
            "kMetal4CachePressureBytes",
            "kMetal4CacheEmergencyBytes",
            "kMetal4CacheEmergencyRecoveryBytes",
            "kMetal4CacheRecoveryBytes",
            "kMetal4CacheShrinkBatch",
            "evictMetal4BaseCacheEntry(impl, victim, true)",
        ):
            if forbidden in source:
                raise AssertionError(f"fixed cache must not shrink for pressure: {forbidden}")

        if not score(20_000_000, 1, 128 * 1024) > score(1_000_000, 1, 128 * 1024):
            raise AssertionError("recreate cost must improve retention value")
        if not score(1_000_000, 16, 128 * 1024) > score(1_000_000, 1, 128 * 1024):
            raise AssertionError("reuse must improve retention value")
        if not score(5_000_000, 2, 64 * 1024) > score(5_000_000, 2, 512 * 1024):
            raise AssertionError("larger measured allocation must reduce retention value")
        if score(U64_MAX, U64_MAX, 0, U64_MAX) != U64_MAX:
            raise AssertionError("score arithmetic must saturate")
        if score(1_000_000, 1, 0) <= 0:
            raise AssertionError("unknown allocation size must never divide by zero")

    except (AssertionError, OSError, UnicodeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1

    print("PASS: Metal 4 fixed-1024 value-aware base-cache source and policy contract")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
