# Cacheless pipeline creation shares device shader libraries

## Problem and repair

The application can deliberately pass `VK_NULL_HANDLE` for `VkPipelineCache`
when a required create cannot acquire a cache's external synchronization lock.
The old no-cache branch in `MVKShaderModule::getMTLFunction` used a private
`MVKShaderLibraryCache` constructed without a shader-module key. Its repository
pointer was null, so an already compiled device library could not be reused.
It also held the module `_accessLock` through conversion/library creation.

For SPIR-V modules, initialize that empty logical view with the completed
module key and the existing device repository before publishing the module.
When the repository is enabled, the no-cache branch now uses the same
`getShaderLibraryConcurrent` implementation as cache-backed creation. Exact
configuration matching, creation coalescing, error handling, and reference
ownership stay in their existing implementation. The module view mutex protects
lookup/publication, not the conversion/compile or creation-gate wait.

Repository-disabled and direct-MSL behavior remain on their existing paths.
Internal reuse without an application VkPipelineCache explicitly clears the
application-cache-hit feedback bit. Cacheless creation does not add membership
to an unrelated application cache or require its lock. No Vulkan API, cache
format, worker policy, or draw-admission change is introduced.

## Deterministic real Vulkan/Metal regression

`Scripts/test-shared-library-vulkan.py --cacheless-reuse` exercises:

- A fresh no-cache request with compilation prohibited returns compile-required.
- Cached → cacheless reuse across distinct VkShaderModule objects with identical SPIR-V.
- Cacheless → cached reuse.
- Concurrent cacheless requests and concurrent mixed cached/cacheless requests.
- Unchanged source cache serialization after no-cache reuse.
- No application-cache-hit feedback when no application cache was supplied.
- GPU execution after all source shader modules and VkPipelineCache objects are destroyed.

The unchanged `8d12fc384` library fails both sharing checks and compiles **8**
libraries. The repair passes and compiles **4**, one per actual input module.
Both produce the correct **80 GPU words**, so the old failure is excess work,
not a deliberately altered rendering result. Repository-disabled mode retains
the expected **8** compiles and correct GPU output.

The original concurrent create/export/merge/roundtrip integration also passes
with 20 pipelines and 80 GPU output words. The existing 13 production-flow
concurrency tests pass individually under ASAN, TSAN and UBSAN. These exercise
the shared implementation reused here; they are not a claim that the complete
Metal driver was sanitizer-instrumented. macOS arm64 Release build passes.

## Application route observation

Managed modules/runtime, input save/cache bytes, Async OFF, Shader/MSL ON, two
pipeline workers, and a 60-second route beginning at game clock 19:00 were held
fixed. The only changed deployed binary was MoltenVK. The test binary was built
from `8d12fc384` plus this source patch, retaining that baseline's real cache UUID;
no cache header was edited. Source SHA256:
`66f86f03167ba2146c195fc3430c824feb68f85ec28f1680234f0cba1f58ec7e`.
Library SHA256:
`eadacd093b68ba90970b68daf3452f73ca389c5f1a839ef2249b55f279e07a44`.

| Measurement | Before | After |
|---|---:|---:|
| Whole-session pipeline creates | 1882 | 1859 |
| Whole-session shader modules | 1453 | 1439 |
| Whole-session Metal library compile tasks | 1739 | 1486 |
| Whole-session pipeline create cumulative ms | 5599.87 | 4714.50 |
| Whole-session join cumulative ms | 4513.43 | 3790.81 |
| Route pipeline creates | 104 | 100 |
| Route pipeline create cumulative ms | 1805.20 | 1277.35 |
| Route join cumulative ms | 1753.13 | 1182.80 |
| Route maximum join ms | 442.57 | 378.68 |
| Route cache lifetime lock wait cumulative ms | 0.0111 | 0.0067 |
| Route sampled mean FPS | 22.30 | 29.51 |
| Route median CPU percent | 310.32 | 351.85 |

The duplicate-work defect is closed by the deterministic native test. The game
route also shows less library work and about 33% less join time in its measured
segment; this single pair is not a universal game/iPhone speedup percentage.
CPU activity rises while frame output also rises, so CPU percent alone is not
per-frame cost. A remaining ~379ms join is not declared fixed. Normal exit and
reviewed endpoints passed; native/MSL jobs settled without recorded compilation,
import, device-loss or unhandled-exception errors.

The post-GPU telemetry retained 8886 guest GPFifo command buffers at stop.
GPFifo Dispose records its remaining guest queue rather than draining it; this
is not the native compilation queue. It is retained as a separate shutdown
observation, not relabelled zero or claimed fixed by library reuse. No iPhone
installation or device-wide lifecycle acceptance was performed.
