# Metal 4 Command Coverage Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Execute representative MeloNX Vulkan submissions on the experimental Metal 4 command backend without weakening whole-submission fallback or legacy correctness.

**Architecture:** Extend the existing two-pass command preflight/materialization surface. Reuse MoltenVK command/resource/pipeline objects, collect and retain all Metal resources before queue side effects, and add command coverage in telemetry-driven checkpoints.

**Tech Stack:** C++17, Objective-C++, Vulkan 1.4, Metal 4/Xcode 26, Python source contracts, macOS Vulkan E2E, MeloNX iOS host-debug harness.

---

### Task 1: Lock first-unsupported-command telemetry

**Files:**
- Modify: `Scripts/test-metal4-command-backend-phase1.py`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommand.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommandPool.mm`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommandBuffer.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm`
- Modify: `Docs/Metal4_Command_Backend_Phase1.md`

1. Add a source-contract assertion requiring stable command type identity, first-blocker propagation, bounded counters, and no per-command log spam.
2. Run `python3 Scripts/test-metal4-command-backend-phase1.py` and confirm it fails for the missing telemetry.
3. Assign the macro-generated command-pool type name to each pooled command object.
4. Return the first unsupported command name from command-buffer preflight and aggregate it in fixed-capacity queue telemetry.
5. Log only first occurrence and power-of-two snapshots; include the leading blocker and count.
6. Rerun the source contract and confirm it passes.
7. Run the full Metal 4 source-contract set and both macOS/iOS builds.
8. Commit the checkpoint.

### Task 2: Support single-queue semaphore submissions

**Files:**
- Modify: `Scripts/test-metal4-command-backend-phase1.py`
- Modify: `Tests/Metal4CommandBackend/metal4_transfer_e2e.cpp`
- Modify: `Tests/Metal4CommandBackend/run-macos.sh`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKSync.h`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKSync.mm`
- Modify: `Docs/Metal4_Command_Backend_Phase1.md`

1. Add failing assertions for `MVKSemaphoreSingleQueue` eligibility and a backend-on E2E run using semaphore style `0`.
2. Prove the new assertions fail before implementation.
3. Mark single-queue semaphores Metal 4-compatible without queue-side wait/signal calls; retain the existing hybrid submission ordering event as the actual cross-backend bridge.
4. Add binary-semaphore, fence, legacy-to-Metal4, Metal4-to-legacy, and present-order checks.
5. Run source contracts, E2E with style `0` and style `2`, and macOS/iOS builds.
6. Commit the checkpoint.

### Task 3: Rank and cover transfer/barrier blockers

**Files:**
- Modify: `Tests/Metal4CommandBackend/metal4_transfer_e2e.cpp`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdTransfer.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdTransfer.mm`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdPipeline.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdPipeline.mm`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm`

1. Capture a host XC3 session and rank the first unsupported command histogram.
2. Select only transfer/barrier commands that form complete submissions in that trace.
3. Add one failing Vulkan output/order assertion per selected command.
4. Implement prepare-time validation/resource collection and minimal Metal 4 encoding.
5. Keep image-layout, host-read, queue-family, and unsupported format cases fail-closed until their semantics are explicitly tested.
6. Rerun all earlier gates and the same XC3 host route; require `real_submissions > 0` before advancing.
7. Commit the checkpoint.

### Task 4: Add retained graphics state and indexed draw

**Files:**
- Modify: `Tests/Metal4CommandBackend/metal4_transfer_e2e.cpp`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommand.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdDraw.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdDraw.mm`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdRendering.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdRendering.mm`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm`

1. Add failing E2E scenes for vertex binding, 16/32-bit index binding, base vertex/instance, viewport, scissor, cull/front-face, and indexed draw.
2. Extend the backend-neutral encoder with state setters and indexed draw.
3. Store dirty state in the Metal 4 materializer and emit it only before draw.
4. Collect vertex/index buffers before execution is claimed and retain their allocations through feedback.
5. Compare rendered pixel checksums between legacy and Metal 4 paths.
6. Run all earlier gates, host XC3, and commit.

### Task 5: Add depth/stencil and multiple render targets

**Files:**
- Modify: `Tests/Metal4CommandBackend/metal4_transfer_e2e.cpp`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdRendering.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdRendering.mm`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm`

1. Add failing E2E scenes for depth compare/write, stencil, two and four color attachments, clear/load/store, and resolve.
2. Reuse existing image-view, render metadata, depth-stencil state, and Metal 4 flexible pipeline objects.
3. Build complete Metal 4 render-pass descriptors and collect every attachment/resolve target for residency.
4. Fail closed on unsupported sample counts, layered rendering, multiview, or attachment mappings until covered by a test.
5. Compare attachment readback and final pixel checksums against legacy.
6. Run all earlier gates, host XC3, and commit.

### Task 6: Materialize common descriptor sets

**Files:**
- Modify: `Tests/Metal4CommandBackend/metal4_transfer_e2e.cpp`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCommand.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdPipeline.h`
- Modify: `MoltenVK/MoltenVK/Commands/MVKCmdPipeline.mm`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKDescriptorSet.h`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKDescriptorSet.mm`
- Modify: `MoltenVK/MoltenVK/GPUObjects/MVKQueue.mm`

1. Add failing E2E coverage for uniform/storage buffers, sampled/storage images, samplers, descriptor arrays, push constants, and dynamic offsets.
2. Define a prepare-time immutable descriptor snapshot owned by the submission.
3. Build Metal 4 argument-table entries from existing MoltenVK binding metadata and collect every referenced resource/view/allocation for residency.
4. Bind the table before compute or render work; preserve descriptor identity and dynamic offsets.
5. Fail closed for update-after-bind, variable descriptor count, unsupported texel buffers, or unbounded arrays until separately tested.
6. Run all gates, require a material increase in XC3 real submissions, and commit.

### Task 7: Final host acceptance and PR

**Files:**
- Modify: `Docs/Metal4_Command_Backend_Phase1.md`
- Create: `Docs/Metal4_Command_Backend_Coverage_Acceptance.md`

1. Build macOS, iOS device, and iOS Simulator targets from a clean derived-data path.
2. Run all source contracts and the full Vulkan E2E with backend off/on.
3. Prepare a new host-debug workspace through `ryujinx-ios-host-debug/tool.py`; do not reuse or overwrite earlier workspaces.
4. Run XC3 from the documented clean profile/save, reach the fixed OCR milestone, execute the same input route, and collect route, first-present, accepted-input, telemetry, and frame evidence.
5. Repeat with command backend disabled. Compare correctness, frame timing, submission coverage, fallback histogram, failures, and memory.
6. Review the complete diff and remove unsupported claims or dead code.
7. Push the stacked base branch and feature branch, then open a new PR against `codex/metal4-pr2-on-pr3` with checkpoint evidence and remaining limitations.
