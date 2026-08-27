# Metal 4 Command Backend Phase 2 Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove the current Metal 4 command submission regression without weakening whole-submission fallback or rendering correctness.

**Architecture:** Optimize the existing correctness-first path in reversible stages. First remove duplicate support scans, then deduplicate allocations during preparation, and only introduce command-buffer caching if measured host telemetry still requires it.

**Tech Stack:** C++17, Objective-C++, Vulkan 1.4, Metal 4/Xcode 26, Python source contracts, macOS Vulkan E2E, MeloNX host-debug harness.

---

### Task 1: Freeze the Phase 1C checkpoint and evaluator

**Files:**
- Create: `docs/plans/2026-08-26-metal4-command-backend-phase2-design.md`
- Create: `docs/plans/2026-08-26-metal4-command-backend-phase2.md`

1. Tag `05f4c19c6` as `checkpoint/metal4-command-phase1c-correctness-20260826` and push the tag.
2. Record the fixed A/C/D host metrics, invariants, evaluator command, and host pass/fail gate in the design.
3. Run `git diff --check` and the three source contracts.
4. Commit the design checkpoint before changing production code.

### Task 2: Remove redundant support scans

**Files:**
- Modify: `Scripts/test-metal4-command-backend-phase1.py`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommandBuffer.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm`

1. Add a source-contract assertion that the queue performs support preflight
   before preparation and that neither `prepareMetal4Encoding()` nor
   `encodeMetal4()` calls `supportsMetal4Encoding()` again.
2. Run `python3 Scripts/test-metal4-command-backend-phase1.py`; expect failure
   identifying the redundant checks.
3. Remove only those two repeated calls; retain null-encoder checks and document
   the preflight prerequisite in the header.
4. Rerun the focused source contract; expect PASS.
5. Run all source contracts, `git diff --check`, macOS build, and Vulkan E2E.
6. Commit as the Phase 2A checkpoint.

### Task 3: Deduplicate allocations during preparation

**Files:**
- Modify: `Scripts/test-metal4-command-backend-phase1.py`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm`

1. Add a source-contract assertion requiring one encoder-owned allocation
   insertion helper and rejecting per-acquire/per-release temporary dedupe sets.
2. Run the focused source contract; expect failure because residency currently
   constructs `unordered_set<uintptr_t> seen` twice.
3. Add a preparation-time allocation-key set and `useAllocation()` helper;
   replace direct `_allocations.push_back()` calls.
4. Make acquire/release iterate the already-unique vector directly while
   retaining the existing mutex, in-flight counts, add/remove calls, and commit
   behavior.
5. Rerun focused and full tests, macOS build, Vulkan E2E, and commit Phase 2B.

### Task 4: Run the fixed XC3 checkpoint

**Files:**
- No product changes.
- Create a new host-debug workspace and evidence directory; never overwrite the
  Phase 1C evidence.

1. Build the exact Phase 2B MoltenVK revision and verify embedded version,
   revision, and SHA-256 in the host workspace.
2. Restore the fixed outdoor save/profile before every run.
3. Run command-only with telemetry off over the exact route and collect 120
   stable samples.
4. Run command-only with telemetry on; require 100% coverage, zero fallback,
   zero failures, unchanged render/draw work, and compare
   support/preparation/residency/encoding timing.
5. Use OCR only for navigation/milestones and run Luna high on every saved
   gameplay screenshot.
6. Reject any title-specific path, skipped frame, missing draw/attachment/query,
   reduced quality, or weakened barrier/synchronization/residency behavior even
   if FPS increases.
7. If FPS is at least -0.5% versus 29.7493, commit/push the evidence checkpoint.
   Otherwise continue to Task 5 without changing the acceptance gate.

### Task 5: Cache immutable eligibility only if required

**Files:**
- Modify: `Scripts/test-metal4-command-backend-phase1.py`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommandBuffer.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm`

1. Audit every `supportsMetal4Encoding()` override and record whether it depends
   only on immutable recorded command data. Stop if any predicate is mutable.
2. Add a failing source/E2E contract for cached eligibility and invalidation on
   begin/reset/rerecord.
3. Store eligibility plus first unsupported command after command-buffer end;
   clear it on reset/begin and never cache preparation resources.
4. Run all automated gates and repeat Task 4.
5. Commit only if the route and timing counters improve without visual or
   fallback regressions.

### Task 6: Final audit

**Files:**
- Modify documentation only if measured results changed the design boundary.

1. Review the diff against the Phase 1C tag.
2. Run the full automated evaluator from a clean build.
3. Confirm the latest host telemetry, route artifacts, and Luna verdicts.
4. Inspect the complete product diff for game-specific conditions, render-work
   suppression, relaxed synchronization, or quality reductions; require none.
5. Confirm the branch and remote point to the verified commit and add the
   measured results to PR #4.
6. Keep persistent prepared-resource/residency caching out of this phase unless
   a separately tested revision and eviction design is completed.
