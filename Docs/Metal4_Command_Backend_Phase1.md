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

## Phase 1A: command-object boundary

Status in this pull request:

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
- residency-set support;
- no Metal private-API build or runtime mode;
- a real device target, not Simulator, tvOS, or visionOS;
- explicit opt-in through the environment variable.

Phase 1A deliberately performs no MTL4 queue commit. It is an object and
lifetime checkpoint, not a command execution or performance checkpoint.

## Phase 1B: submission ownership and allocator arena

Phase 1B may begin only after Phase 1A builds and runs cleanly. It introduces:

- `MVKMetal4SubmissionContext`, with no raw callback capture of `MVKDevice` or
  queue-owned state;
- an allocator arena under each Vulkan command pool or queue, because one
  allocator cannot encode multiple command buffers concurrently;
- one materialized `MTL4CommandBuffer` per in-flight Vulkan command-buffer
  execution;
- conservative residency attachment;
- empty and fence-only submission support;
- MTLEvent/MTLSharedEvent ordering for queue waits and signals;
- completion tokens that retain every object needed until GPU completion;
- whole-submission fallback selected before any Metal 4 encoding starts.

Phase 1B must not replay a Vulkan submission on legacy after an MTL4 command
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
3. Completion callbacks own only self-contained retained state.
4. Device or queue destruction cannot race a callback that dereferences either.
5. Residency removal is deferred until every referencing submission completes.
6. Descriptor and argument-table versions remain immutable while in flight.
7. Legacy and MTL4 queues synchronize through monotonically increasing events.
8. Debug or telemetry callbacks run outside internal queue, residency, allocator,
   and descriptor locks.
9. No MTL4 failure causes duplicate Vulkan side effects.
10. Toggle-off and unsupported targets remain byte-for-byte on the legacy
    execution path except for compiled dead code.

## Validation gates

### Phase 1A

- `python3 Scripts/test-metal4-command-backend-phase1.py`
- Xcode 26 macOS Release build.
- Xcode 26 iOS device Release build.
- Older deployment targets continue compiling through availability guards.
- Toggle off: no Metal 4 command object creation log.
- Toggle on, eligible device: one ready log per Vulkan queue.
- Toggle on, ineligible target: one diagnostic and unchanged Vulkan behavior.
- Repeated `VkDevice` create/destroy loop under Metal API Validation.

### Phase 1B

- Empty submit and fence-only submit tests.
- Binary and timeline semaphore ordering tests.
- Multiple in-flight command buffers from distinct threads.
- Allocator lease/release and teardown race tests.
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
