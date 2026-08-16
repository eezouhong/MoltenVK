# Metal 4 Command Backend — Phase 1

## Base and goal

This work starts from exact commit:

```text
4de6c0e31035e434f2f73ce12148d8ca770922c3
```

The goal is to migrate Vulkan command execution to Metal 4 without changing the
Vulkan application. Ryujinx and other clients continue to create, record, and
submit ordinary Vulkan queues and command buffers. MoltenVK selects the Metal
backend internally.

```text
Vulkan application
    VkQueue / VkCommandPool / VkCommandBuffer / VkSubmitInfo
                              |
                              v
MoltenVK command representation and backend selection
                  |                         |
                  v                         v
       legacy Metal backend       Metal 4 command backend
```

This document divides Phase 1 into checkpoints that can be reviewed and
validated independently. A checkpoint must not claim performance improvements
until real Vulkan commands execute on the Metal 4 queue.

## Current checkpoint status

| Checkpoint | Status | What is real |
|---|---|---|
| Phase 1A | Implemented; build/device validation pending | MTL4 queue, allocator, and command-buffer creation plus begin/end lifecycle |
| Phase 1B.1 | Implemented; build/device validation pending | Independent command residency, bounded allocator arena, one internal empty MTL4 commit, feedback completion, allocator reset, teardown-safe late callback |
| Phase 1B.2 | Not implemented | Vulkan fence/semaphore/event submission ownership, resource collection, and queue-to-queue ordering |
| Phase 1C | Not implemented | Real Vulkan transfer, compute, render, descriptor, barrier, and present commands on MTL4 |

All ordinary Vulkan submissions and presentation remain on the legacy Metal
queue at the current checkpoint.

## Phase 1A: command-object boundary

Implemented in this pull request:

- Add a private, default-off `MVK_CONFIG_METAL4_COMMAND_BACKEND` experiment.
- Create one `MTL4CommandQueue` sidecar for each eligible `MVKQueue`.
- Validate creation and begin/end lifetime of an `MTL4CommandAllocator` and
  `MTL4CommandBuffer`.
- Expose requested/ready state to later MoltenVK command-backend code.
- Preserve all Vulkan submission, semaphore, fence, present, completion, and
  device-loss behavior on the existing legacy queue.
- Fall back to legacy when any eligibility or object-creation check fails.
- Keep the experiment out of the public `MVKConfiguration` ABI.

Eligibility requires all of the following:

- Xcode 26 SDK build support;
- macOS 26 or iOS 26 runtime;
- a Metal 4-capable GPU;
- Metal residency-set capability;
- no Metal private-API build or runtime mode;
- a real device target, not Simulator, tvOS, or visionOS;
- explicit opt-in through the environment variable.

The command backend does not depend on MoltenVK's legacy argument-buffer
residency set. Each Metal 4 queue owns an independent residency set through its
shared command state, so disabling the legacy argument-buffer path does not
disable the Metal 4 command backend.

## Phase 1B.1: residency, allocator arena, and internal commit probe

Implemented in this pull request:

- A command-backend-owned `MTLResidencySet`, initially bounded to 256 entries,
  committed and attached to the `MTL4CommandQueue`.
- The shared command state retains the residency set through any late commit
  feedback. Queue teardown marks the state shutting down, detaches the set,
  releases the queue, and only then releases its own shared-state reference.
- A bounded allocator arena, defaulting to four allocators and hard-limited to
  sixteen.
- A private `MVK_CONFIG_METAL4_COMMAND_ALLOCATOR_COUNT` override.
- Explicit separation of allocator `encoding`, `inFlightCount`, and
  `resetPending` state.
- An allocator is never reset while any command buffer encoded from it remains
  in flight.
- One empty, internal-only `MTL4CommandBuffer` is committed to each eligible
  Metal 4 queue.
- `MTL4CommitOptions` and `MTL4CommitFeedback` verify actual GPU completion.
- The feedback block captures only a shared queue-independent state object and a
  slot index. It does not capture or dereference `MVKQueue`, `MVKDevice`, or the
  legacy submission object.
- Objective-C exceptions during residency, command-buffer setup, or commit fail
  the probe and retain the legacy Vulkan path.

This empty commit is the first real use of `MTL4CommandQueue`, but it is not a
Vulkan workload and is not a performance result.

## Phase 1B.2: Vulkan submission ownership

The next submission checkpoint introduces:

- `MVKMetal4SubmissionContext`, with no raw callback capture of `MVKDevice` or
  queue-owned state;
- a materialized `MTL4CommandBuffer` per in-flight Vulkan command-buffer
  execution;
- collection of buffers, images, heaps, pipeline states, argument tables, and
  temporary resources referenced by a submission;
- batched updates of the command residency set before commit, with removal
  deferred until all referencing submissions complete;
- empty and fence-only Vulkan submission support;
- MTLEvent/MTLSharedEvent ordering for queue waits and signals;
- completion tokens that retain every object needed until GPU completion;
- whole-submission fallback selected before any Metal 4 encoding starts.

Phase 1B.2 must not replay a Vulkan submission on legacy after an MTL4 command
buffer has been committed. Post-commit failure is a submission/device error, not
a fallback opportunity.

## Phase 1C: first real command execution

The first execution slice is intentionally narrow:

1. buffer fill and buffer copy;
2. compute dispatch;
3. image copy/clear paths that map to `MTL4ComputeCommandEncoder`;
4. ordinary single-view rendering and dynamic rendering;
5. drawable wait/signal and present;
6. basic Vulkan synchronization2 barriers;
7. argument-table materialization for the descriptor types used by the selected
   smoke workload.

The initial execution slice excludes:

- tessellation;
- multiview;
- geometry or transform-feedback emulation;
- secondary command buffers;
- simultaneous-use materialization caching;
- sparse resources;
- query/timestamp migration;
- archive or pipeline-binary persistence;
- ray tracing and machine-learning encoders.

If an unsupported command exists in a Vulkan submission, the entire submission
is selected for legacy before encoding begins. Command-level switching inside a
submission is not permitted in Phase 1.

## Application boundary

No Ryujinx Vulkan command-recording changes are required for Phase 1. Application
changes, if any, are limited to:

- the opt-in toggle;
- capability display;
- telemetry and crash diagnostics;
- allow/deny policy for experimental builds.

The application must not duplicate descriptor-resource or residency reporting.
MoltenVK already observes Vulkan resource creation, memory binding, descriptor
updates, pipeline resource usage, command recording, queue ordering, and object
lifetime. Phase 1 uses a conservative residency superset where exact inference
is not yet available.

## Safety invariants

Every checkpoint must preserve these invariants:

1. Backend selection occurs before encoding a Vulkan submission.
2. A command buffer is never encoded simultaneously with the same allocator.
3. An allocator is reset only after all command buffers encoded from it finish.
4. Completion callbacks own only self-contained retained state.
5. Device or queue destruction cannot race a callback that dereferences either.
6. Residency removal is deferred until every referencing submission completes.
7. Descriptor and argument-table versions remain immutable while in flight.
8. Legacy and MTL4 queues synchronize through monotonically increasing events.
9. Debug or telemetry callbacks run outside internal queue, residency, allocator,
   and descriptor locks.
10. No MTL4 failure causes duplicate Vulkan side effects.
11. Toggle-off and unsupported targets remain on the legacy execution path.

## Validation gates

### Phase 1A / 1B.1

- `python3 Scripts/test-metal4-command-backend-phase1.py`
- Xcode 26 macOS Release build.
- Xcode 26 iOS device Release build.
- Private-API builds compile while excluding Metal 4 command execution.
- CMake Debug and Release builds.
- Older deployment targets continue compiling through availability guards.
- Toggle off: no Metal 4 command object or commit-probe log.
- Toggle on, eligible device: queue, independent residency, allocator readiness,
  and one completed empty commit probe per Vulkan queue.
- Toggle on with legacy argument buffers disabled: the independent Metal 4
  residency set is still created and the empty commit probe completes.
- Toggle on, ineligible target: one diagnostic and unchanged Vulkan behavior.
- Repeated `VkDevice` create/destroy loop under Metal API Validation.
- Destroy the queue/device immediately after the empty commit and verify late
  feedback does not dereference queue/device state.

### Phase 1B.2

- Empty and fence-only Vulkan submit tests.
- Binary and timeline semaphore ordering tests.
- Multiple in-flight command buffers from distinct threads.
- Allocator lease/release and teardown race tests.
- Resource residency add/remove and aliasing tests.
- Device-loss and failed object-creation injection.
- No callback use-after-free under Thread Sanitizer where supported.

### Phase 1C

- Vulkan CTS subsets for command buffers, synchronization, transfer, compute,
  dynamic rendering, descriptors, and swapchain.
- Metal API Validation with no errors.
- Identical screenshots/readbacks against legacy for deterministic fixtures.
- In-place iOS game smoke with the same Ryujinx executable and only the backend
  toggle changed.
- Long-run memory, command latency, p99 frame time, thermal, and device-loss
  telemetry.

## Performance gate

Creating newer API objects is not itself a success. Continue beyond Phase 1 only
when a representative iOS workload demonstrates at least one of:

- a material reduction in MoltenVK CPU command encoding and submission time;
- fewer encoder transitions from unified compute/blit work;
- lower command-buffer allocation churn;
- measurable improvement in CPU-bound frame time or frame pacing.

A regression in correctness, p99 frame time, memory pressure, thermal behavior,
or long-run stability blocks promotion regardless of average FPS.
