# Metal 4 Command Coverage Design

## Objective and safety boundary

Extend the experimental Metal 4 command backend until representative MeloNX
Vulkan submissions can execute on `MTL4CommandQueue`, while preserving the
existing Metal backend as a complete runtime fallback. The work is stacked on
`codex/metal4-pr2-on-pr3`, remains default-off, and does not change the public
`MVKConfiguration` ABI.

The defining invariant is whole-submission fail-closed selection. Every command,
semaphore, resource, render attachment, descriptor, and synchronization effect
must be validated and retained before the first Metal 4 queue-side effect. If
preparation cannot prove support, the complete Vulkan submission uses the
legacy backend. After a Metal 4 queue wait or commit, an error is not replayed
on legacy because that could duplicate Vulkan side effects.

The implementation reuses MoltenVK's Vulkan command objects, shader and
pipeline compilation, resource wrappers, render metadata, and existing hybrid
queue ordering. It adds a sibling Metal 4 materializer rather than a second
Vulkan frontend. Presentation can remain on the legacy queue while the shared
ordering event bridges Metal 4 and legacy submissions.

## Phased architecture

Phase 1 adds bounded blocker telemetry and closes the single-queue semaphore
gap. Each command pool assigns a stable command type name to its command
objects. Command-buffer preflight returns the first unsupported type, and queue
telemetry counts it without logging every command. The `SINGLE_QUEUE` semaphore
style becomes eligible only under the existing one-Vulkan-queue contract and
uses the already implemented hybrid submission sequence for legacy/Metal 4
ordering.

Phase 2 expands low-state command coverage according to real XC3 blocker data:
transfer variants, image barriers that can be represented without unsafe host
effects, and state-only commands whose values can be retained until a draw.

Phase 3 completes an ordinary graphics path: viewport/scissor and raster state,
vertex/index binding, indexed draw, depth/stencil, multiple color attachments,
load/store/clear, and resolve. Metal resources are collected in the prepare
pass, while the encoding pass maintains dirty state and emits it at draw time.

Phase 4 materializes Vulkan descriptor sets as Metal 4 argument tables. The
prepare pass snapshots descriptor identity, applies dynamic offsets, collects
buffers/textures/samplers/views for residency, and retains everything until
commit feedback. Unsupported update-after-bind, variable-count, or layout
cases remain fail-closed until individually covered.

## Checkpoints and validation

Every phase starts with a failing source-contract or Vulkan E2E assertion,
implements only the required behavior, then reruns the focused test, macOS and
iOS builds, and the previous phase's E2E. No later phase starts while an earlier
gate is red.

The macOS Vulkan E2E compares backend-off and backend-on output and validates
buffer, image, compute, render, semaphore, and ordering results. The private
host harness then builds MeloNX against the exact MoltenVK commit and exercises
the fixed iOS-like route on Apple Silicon. Structured route, first-present,
accepted-input, and Metal 4 telemetry are primary evidence; Apple Vision OCR is
used for XC3 milestones, with image-model inspection only when OCR cannot
classify the screen.

Host validation proves shared-core and MoltenVK behavior, not iPhone driver,
thermal, memory-pressure, NativeAOT, or `CAMetalLayer` equivalence. Final PR
acceptance requires a clean diff, source contracts, device-target compilation,
host A/B evidence from the same XC3 save and route, no Metal 4 failures or
device loss, and explicit documentation of any commands still falling back.
