# Metal 4 Command Backend Phase 2 Optimization Design

## Objective and fixed evidence

Phase 1C correctness and telemetry is frozen at tag
`checkpoint/metal4-command-phase1c-correctness-20260826` (`05f4c19c6`). The
fixed host baseline is the restored Xenoblade Chronicles 3 outdoor,
no-monster route ending at Rhogett Causeway. Over 120 stable samples, the
all-off baseline averaged 29.7493 FPS and the command-only, telemetry-off run
averaged 29.3405 FPS (-1.374%). A telemetry run proved 100% real Metal 4
coverage with zero fallbacks and zero failures.

Phase 2 succeeds only when the same route is no worse than -0.5% versus that
fixed all-off baseline, the automated build/E2E evaluator passes, telemetry
still reports 100% coverage with zero fallback/failure, and Luna high finds no
rendering defect in every saved gameplay screenshot. A positive result remains
the target, but correctness and iPhone memory safety are not traded for it.

## Invariants

- Whole-submission fail-closed selection remains before any Metal 4 queue side
  effect.
- A pre-commit error may replay on the legacy backend; a post-commit error may
  not replay.
- Command buffers may be rerecorded or reset without stale eligibility state.
- Descriptor contents and other mutable Vulkan resources are not cached without
  an explicit revision/lifetime contract.
- Residency accounting counts each Metal allocation once per submission and
  releases exactly what it acquired.
- The command backend remains private, default-off, and independently
  switchable from the Metal 4 compiler/flexible-pipeline feature.
- Every optimization is backend-general. No game/title/route special case,
  workload fingerprint, or benchmark-only fast path is allowed.
- No frame skipping, render suppression, resolution/quality reduction, missing
  draw, attachment, query, barrier, or synchronization work is accepted as a
  performance improvement.
- Vulkan ordering, visibility, resource lifetime, and Metal residency semantics
  remain unchanged. A faster result caused by weaker validation or silently
  discarded work fails the phase.

## Considered approaches

### 1. Remove redundant work inside the existing submission pipeline

Keep the correctness architecture and eliminate work that is provably repeated:
the queue performs one whole-submission support preflight, so preparation and
encoding do not rescan support; the preparation encoder produces a unique
allocation list, so residency acquire/release do not build new deduplication
sets. This is the recommended first approach because it is small, reversible,
and preserves every fallback boundary.

### 2. Cache immutable command-buffer classification

Compute Metal 4 eligibility once for a completed command-buffer recording and
invalidate it on begin/reset/rerecord. This moves support classification out of
the high-frequency submit path. It is safe only after source review proves that
all `supportsMetal4Encoding()` implementations depend exclusively on immutable
recorded command state.

### 3. Persist preparation and residency state

Cache resolved resources/descriptor snapshots or leave allocations resident
between submissions. This offers the largest theoretical gain but has the
largest correctness and memory risk: descriptors can be updated, image/buffer
backing can change, and retained residency can increase iPhone memory pressure.
It is deferred until Phase 2A/2B telemetry shows it is necessary and a concrete
revision plus eviction contract exists.

## Selected staged design

Phase 2A removes the two redundant support scans from
`prepareMetal4Encoding()` and `encodeMetal4()`. The queue's successful
whole-submission preflight becomes the documented prerequisite. A source
contract must fail before the code change and must continue requiring preflight
before preparation.

Phase 2B adds one encoder-owned allocation insertion helper. It deduplicates as
resources are collected, preserving first-seen order. Queue residency code then
operates on the unique vector without allocating two temporary
`unordered_set`s. Tests lock unique input and symmetric acquire/release.

After each phase, run source contracts, a clean macOS build, Vulkan E2E, and the
same XC3 route. Continue only if correctness remains green and the targeted
timing counter improves. If Phase 2A and 2B do not meet the route gate, review
all support predicates for immutable-only dependencies and implement the
command-buffer eligibility cache as Phase 2C. Persistent preparation/residency
requires a separate checkpoint and is not authorized by performance evidence
alone.

## Evaluator contract

Automated evaluator:

```sh
python3 Scripts/test-metal4-base-cache-policy.py && \
python3 Scripts/test-metal4-flexible-pipeline.py && \
python3 Scripts/test-metal4-command-backend-phase1.py && \
bash -n Tests/Metal4CommandBackend/run-macos.sh && \
git diff --check && \
make macos && \
bash Tests/Metal4CommandBackend/run-macos.sh
```

Host gate: restore the fixed save/profile, run the same outdoor route with
compiler/flexible features off and command backend on, collect 120 stable
telemetry-off samples, then repeat with telemetry on. Require FPS delta at
least -0.5%, `coverage_ppm=1000000`, `fallbacks=0`, `failures=0`, identical OCR
route milestones, unchanged frame/render/draw work counters where observable,
and Luna-high PASS for each saved screenshot. Any evidence of skipped frames,
missing rendering, reduced quality, weakened synchronization, or title-specific
behavior fails the evaluator regardless of FPS. The private
host harness is shared-core evidence; physical iPhone validation remains the
final device gate.
