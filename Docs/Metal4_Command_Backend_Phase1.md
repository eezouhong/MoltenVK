# Metal 4 Command Backend: Phase 1

## Goal

Build a Metal 4 command backend behind MoltenVK's existing Vulkan object and
`MVKCommand` model. Ryujinx and other Vulkan applications must continue to
record and submit the same Vulkan command buffers.

The first checkpoint in this branch establishes the command transport that all
later encoder work depends on:

- a separately gated `MTL4CommandQueue`;
- a bounded allocator arena because one allocator cannot encode multiple
  command buffers concurrently;
- a real empty `MTL4CommandBuffer` commit;
- bounded commit-feedback validation;
- callback ownership that remains safe after a validation timeout;
- fail-closed fallback to the existing Metal backend.

This checkpoint does **not** redirect Vulkan command buffers to Metal 4 yet.
Legacy Metal command encoding and submission remain authoritative until a full
Vulkan submission has been classified as supported before encoding starts.

## Runtime gate

The experiment is disabled by default:

```text
MVK_CONFIG_METAL4_COMMAND_BACKEND=1
```

Optional internal tuning variables:

```text
MVK_CONFIG_METAL4_COMMAND_ALLOCATOR_MAX=4
MVK_CONFIG_METAL4_COMMAND_VALIDATION_TIMEOUT_MS=5000
```

The allocator count is clamped to 1-16. Validation timeout is clamped to
100-30000 ms.

The command transport is rejected before use when any of these conditions is
true:

- the build or runtime enables Metal private APIs;
- the target is tvOS, visionOS, or a simulator in this experiment;
- the SDK is older than Xcode 26;
- the OS or GPU does not support Metal 4;
- a required Metal 4 factory is unavailable;
- queue or allocator creation fails;
- the validation commit times out, returns no feedback, or returns an error.

None of those cases changes the Vulkan result or disables the existing Metal
queue.

## Ownership and timeout policy

Commit feedback captures only a heap-owned result context. That context retains
the Metal 4 queue, command buffer, and allocator used by the validation commit.
It does not retain or dereference `MVKQueue` or `MVKDevice` after submission.

If Metal never calls the feedback block, one bounded validation context can
remain retained by Metal. The transport is disabled, no additional Metal 4
command work is accepted, and the legacy backend continues normally.

## Migration stages

### Phase 1A: command transport — implemented here

- queue creation and capability gating;
- allocator leasing and teardown wake-up;
- command-buffer begin/end;
- grouped queue commit API;
- feedback and failure telemetry;
- source contract test.

### Phase 1B: compute and transfer encoding

- materialize supported primary Vulkan command buffers;
- migrate compute dispatch, buffer/image copies, fill, clear, and resolve to
  `MTL4ComputeCommandEncoder`;
- classify the complete `VkSubmit` before encoding;
- fall back for the entire submit when it contains an unsupported command;
- preserve semaphore, fence, and queue ordering with events.

### Phase 1C: render and presentation

- ordinary render and dynamic rendering;
- Metal 4 color attachment mapping from the existing flexible pipeline work;
- argument-table snapshots and conservative residency;
- drawable wait/signal and presentation;
- end-to-end game A/B telemetry.

### Phase 2: production coverage

- descriptor indexing, dynamic offsets, update-after-bind, and push data;
- secondary and reusable command buffers;
- simultaneous use with multiple materializations;
- complete synchronization2 mapping;
- queries, timestamps, indirect commands, MSAA, and multiview policy;
- static plus transient residency sets.

### Phase 3: advanced paths

- render suspend/resume and parallel encoding;
- texture view pools and fine-grained residency;
- transform feedback and geometry emulation;
- tessellation-to-mesh only if game traces justify it;
- archive, sparse placement, and pipeline binaries only after independent
  measurements justify them.

## Non-negotiable fallback boundary

Fallback is allowed only before an entire Vulkan submission begins Metal
encoding. A committed or partially encoded Metal 4 submission is never replayed
through legacy Metal, because doing so can repeat GPU-visible side effects.

## Acceptance gate for redirecting Vulkan work

The transport alone is not a performance claim. Redirecting non-empty Vulkan
command buffers requires all of the following:

1. Xcode 26 macOS and iOS builds pass with the experiment on and off.
2. Metal API Validation reports no errors.
3. The legacy path remains byte-for-byte selectable.
4. A supported submit produces equivalent output and synchronization.
5. Unsupported submits are rejected before any Metal 4 encoding.
6. Process memory, allocator counts, feedback contexts, and residency remain
   bounded in long-running tests.
7. Device-game telemetry demonstrates a measurable CPU or frame-time benefit.
