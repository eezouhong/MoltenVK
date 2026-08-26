# Metal 4 Flexible Pipeline POC

## Scope

This branch evaluates a guarded, unified Metal 4 compiler path on top of
`v1.4.2-ryujinx.6`. It compiles MSL libraries, ordinary flexible render
pipelines, and standalone compute pipelines through one device-owned compiler.
It does not replace MoltenVK's Vulkan command model or SPIR-V/MSL pipeline cache.

The experiment is disabled by default. Set both `MVK_CONFIG_METAL4_COMPILER=1`
and `MVK_CONFIG_METAL4_FLEXIBLE_PIPELINES=1` before creating the Vulkan device
to request it. Unsupported platforms and devices go directly to legacy without
calling Metal 4. On eligible devices, a real library, render, or compute failure
trips only that lane's device-lifetime breaker and makes one legacy attempt in
the same Vulkan creation call. Later requests in that lane bypass Metal 4.

The experiment uses only public Metal APIs. It does not use `MTL4Archive`,
`MTLBinaryArchive`, Metal 4 command queues/buffers/encoders, private APIs,
residency sets, or sparse placement.

## Pipeline boundary

MoltenVK continues to translate SPIR-V to MSL and to reuse the existing Vulkan
pipeline-cache conversion records. On a source-cache hit, the restored MSL and
its existing `MTLCompileOptions` populate an `MTL4LibraryDescriptor`; on a miss,
the same happens after SPIRV-Cross emits MSL. The shared compiler returns a
normal `MTLLibrary`, so existing function-constant and cache ownership remains.

A device-owned `MTL4Compiler` serves library, render, and compute creation under
one device-capped slot budget. Its render lane holds a bounded cache of
unspecialized base render pipelines. A base key contains every fixed state:

- vertex and fragment library/function/specialization identities;
- the complete Metal vertex descriptor;
- primitive topology, sample count, rasterization, alpha coverage/one,
  vertex-amplification, indirect-command-buffer support, and shader validation.

Color attachment pixel format, blend enable, write mask, six blend factors,
and two blend operations are deliberately excluded from the base key. All of
those fields are marked unspecialized on the base and are copied from the
existing legacy descriptor into the final specialization descriptor.

The flexible render lane accepts only ordinary non-tessellated, non-multiview
render pipelines. Existing tessellation, multiview, helper, and failure
semantics remain unchanged. Standalone compute uses an
`MTL4ComputePipelineDescriptor`; tessellation's internal compute stages remain
legacy. The base cache is bounded to at most 1024
resident entries. `MVK_CONFIG_METAL4_FLEXIBLE_CACHE_POLICY=0` uses LRU;
`=1` uses measured compile cost, reuse frequency, and public Metal allocation
size for value-aware admission and eviction. A fixed ghost ring retains only
bounded history. Low-frequency public available-memory samples remain telemetry
only; this device experiment does not shrink the resident target under pressure.

## Function constants

The existing `MVKShaderLibrary` is the only place that knows which
`MTLLibrary`, entry-point name, and Vulkan specialization constants produced an
`MTLFunction`. It therefore also creates the matching public
`MTL4LibraryFunctionDescriptor` or `MTL4SpecializedFunctionDescriptor` and a
stable process-local key. A Metal 4 pipeline must never reconstruct a function
from its name alone when function constants were used.

## Color attachment mapping

For an eligible Metal 4 pipeline, the base descriptor uses inherited logical
to physical color attachment mapping. MoltenVK leaves the render-pass
attachments in their original physical order and binds an
`MTLLogicalToPhysicalColorAttachmentMap` before the draw. The map is derived
from `VkRenderingAttachmentLocationInfo`: each Vulkan color attachment index is
the physical Metal slot and its location value is the logical shader output.

Render-pass descriptors opt into mapping only while the experimental device
service is active. Switching to a legacy pipeline binds a nil map, and legacy
remapped pipelines continue to use MoltenVK's existing render-pass restart and
attachment-rewrite path. If `vkCmdSetRenderingAttachmentLocations` is recorded
after a Metal 4 pipeline bind, MoltenVK keeps the Metal render-pass attachments
in physical order; Vulkan requires the command-time mapping to match the bound
pipeline, so the pipeline-owned Metal map remains the single remapping step.

## Lifetime, concurrency, and failure policy

The compiler service and base cache live for one `MVKDevice`. Library, render,
and compute operations share one concurrency counter. Its effective ceiling is
one when asynchronous compilation is disabled; otherwise it is
`min(clamp(MVK_CONFIG_METAL4_FLEXIBLE_ASYNC_MAX, 1, 3),
maximumConcurrentCompilationTaskCount)`. A task releases its slot before a
dependent stage starts. Cache entries coordinate concurrent creators so a key
is compiled once. Failed keys are recorded in the
bounded ghost ring and immediately use the legacy path on later requests. Successful
specialized pipeline states remain owned by their normal `MVKGraphicsPipeline`
objects; evicting a base affects only future specialization opportunities.

All Metal 4 compilation uses the public asynchronous task API even when the
configured concurrency is one. Calls wait for a bounded interval: a finite
`MVK_CONFIG_METAL_COMPILE_TIMEOUT` is preserved, while MoltenVK's effectively
unbounded default becomes 30 seconds for this experimental service. A task
timeout is treated as a shared-compiler failure, opens all three Metal 4 lane
breakers, wakes slot/cache waiters, and lets the originating Vulkan call make
its single legacy attempt. A base-cache coalescing or pending-capacity wait
timeout is only a coordination delay: it records separate telemetry and uses
legacy for that Vulkan call without opening the render breaker. Metal exposes
no task-cancellation API. A late completion therefore owns only a small heap
result context and retained Metal
objects; it never calls back into `MVKDevice` or the compiler service. If Metal
never completes, at most the already in-flight task contexts remain retained,
and the fatal breaker prevents further accumulation.

Service destruction marks shared state as shutting down and wakes all waiters
without waiting forever for Metal. Base-cache capacity waiters recheck the key
after every wake and checked insertion ensures only one owner compiles or
updates resident accounting. Telemetry is formatted from a mutex-protected
snapshot, then emitted after releasing the mutex so application debug callbacks
cannot participate in a cache-lock inversion.

The device logs per-lane attempts, queue wait, task time, breaker state,
fallback/direct-legacy counts, base hits/misses, specialization successes,
fallbacks, total/maximum base and specialization compile times, value-policy
admission decisions, measured resident base-PSO bytes, and
available-memory snapshots when the experiment is enabled. Measured PSO bytes
do not include retained descriptors, driver-internal caches, or final
specialized pipelines. It also reports legacy graphics, tessellation-stage,
and standalone compute compiler counts, failures, total time, and maximum time,
so an ineligible legacy path cannot be mistaken for a Metal 4 base miss or
specialization. These diagnostics do not change the public MoltenVK
configuration ABI or Vulkan pipeline cache data.

## Validation and stop conditions

Local macOS validation must prove all of the following before an iOS build is
considered:

1. Xcode 26 compiles every normal MoltenVK platform target with older deployment
   targets preserved by availability and SDK guards.
2. The macOS Cube demo renders with the experiment both off and on.
3. Logs show at least one Metal 4 base creation and specialization when on, and
   only legacy compilation when off.
4. Metal API Validation reports no descriptor, render-pass, or mapping errors.
5. A failure injection reaches the legacy compiler without changing the Vulkan
   result.

This POC is not an iOS performance claim. Enabling it in an application
requires separate in-place iOS 26 device validation that preserves the
existing app and its data container.

## Local validation result

The original flexible-render POC passed the source contract, macOS package
builds, iOS arm64 and Simulator compilation, and the macOS Vulkan Cube demo.
The unified library/render/compute extension subsequently passed a fresh public
iOS Release build from clean `v1.4.2-ryujinx.6` plus repository patches. The
Simulator result remains compile-only because MoltenVK deliberately excludes
Metal 4 runtime support from Simulator builds.

The earlier enabled Cube run created one flexible base and one specialization without a
Metal validation error. The final diagnostic sample measured 1,631,416 ns for
that base and 101,125 ns for its specialization. A separate two-pipeline run
produced one base miss and one base hit, and a forced-error run rendered through
the legacy compiler with one recorded fallback. Those numbers validate the
preserved flexible-render checkpoint; the unified library and compute lanes
still require an in-place iOS 26 game run before any performance claim.
