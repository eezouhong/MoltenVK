# Metal 4 shared shader-library repository

This experimental Metal 4 path separates Vulkan pipeline-cache membership from
physical Metal shader-library ownership.

## Ownership model

Each `VkPipelineCache` still owns an independent logical view. Serialization and
`vkMergePipelineCaches` operate on that view, preserving Vulkan-visible cache
semantics. Matching entries from the global cache and per-program MSL caches
point to one device-owned `MVKShaderLibrary` instead of retaining duplicate
`MTLLibrary`, compressed MSL, conversion-result metadata, and specialization
state. Each logical view still retains its own conversion-configuration
membership record in this phase.

The physical key is:

- `MVKShaderModuleKey`
- matching `SPIRVToMSLConversionConfiguration`

Runtime SPIR-V conversion and persisted-MSL import may both run:

- If persisted MSL wins, later runtime/global lookups reuse the canonical library.
- If runtime conversion wins, later MSL imports add only logical membership.
- If they finish concurrently, only the first successful candidate is published.
  The loser is destroyed after the repository lock is released.
- If the canonical entry was already reclaimed, a later successful runtime or
  MSL candidate transfers its compiled `MTLLibrary` payload into that cold
  canonical entry instead of being discarded and compiled a second time.

## Fine-grained resident reclaim

Logical membership keeps the cold, serializable record alive:

- compressed MSL
- shader conversion result metadata
- cache-view membership

The expensive resident payload can be reclaimed independently:

- `MTLLibrary`
- macro-specialized library variants
- Metal 4 library content key derived from the resident payload

When the shared repository is enabled, graphics pipelines release their
temporary Metal 4 function descriptors as soon as synchronous pipeline
construction finishes. The repository-disabled path keeps the RC6 descriptor
lifetime unchanged. This prevents every live `VkPipeline` from pinning its
source library. The separately bounded Metal 4 compiler base cache keeps its
input descriptors only while a base compile is in flight, then releases them
when the shared repository is enabled; later specialization is derived from the
compiled base pipeline.

Reclaim is per canonical shader-library entry, not per global cache and not per
program cache. It uses approximate LRU order and a non-blocking per-library
`try_lock`, so an entry actively selecting or publishing a resident/macro
variant is skipped rather than stalled. Once the selected `MTLLibrary` is
locally retained, Metal function retrieval and function-constant specialization
run outside the per-library lock, preventing shared ownership from serializing
the full foreground and background pipeline paths. A short active-use lease
keeps trim from marking the canonical entry cold while that unlocked Metal work
is still in progress.

LRU advances only when a library actually produces a function. MSL import and
logical cache-view attachment do not make an unused entry look hot. A small
high-water margin batches reclaim instead of sorting the repository after every
single new resident entry.

A later normal pipeline request rehydrates the resident payload from compressed
MSL. A request carrying
`VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT` does not rehydrate,
and does not create a missing macro-specialized library variant; it returns
`VK_PIPELINE_COMPILE_REQUIRED` through the existing pipeline path.

Reclaim drops the repository-owned Metal-library reference after construction-
only pipeline descriptors and completed base-cache input descriptors have been
released. Metal may still retain internal objects through the compiled base or
specialized pipeline beyond the last visible Objective-C library reference.
Device A/B must therefore measure actual footprint rather than treating the
resident-entry counter as released bytes.

## Configuration

The repository is configured through the normal MoltenVK configuration ABI:

```text
MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_REPOSITORY_ENABLED=0|1
MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_RESIDENT_LIMIT=<entry count>
```

The repository is disabled by default for generic MoltenVK users. When enabled,
a resident limit of `0` provides physical deduplication without resident
eviction; a positive value adds the approximate resident-entry limit. Apps
using the private configuration API can set both appended `MVKConfiguration`
fields directly before creating the `VkDevice`.

This explicit enable bit preserves the RC6 binary baseline when the feature is
not requested while making a dedupe-only limit of `0` unambiguous.

## Phase-1 limits

- The resident budget is entry-count based, not byte based. Serialized size and
  entry count are diagnostics; `phys_footprint` remains the memory source of
  truth.
- The existing MeloNX MSL worker and its scheduling are unchanged. A very low
  positive limit can prewarm an entry and reclaim it before first use, so device
  testing should start with `0`, then step through `2048`, `1024`, and `512`.
- The repository removes long-lived duplicate ownership. It does not cancel two
  candidates that are already compiling concurrently; it publishes or adopts
  one physical result and releases the other after completion.
- Logical pipeline-cache views still retain their own conversion-configuration
  records. Phase 1 deduplicates the expensive physical library payload, not
  every byte of view metadata.

## Required validation

1. Xcode 26 iOS and macOS package builds, with and without private APIs.
2. CMake Debug and Release builds.
3. `Scripts/test-metal4-shared-shader-repository.py`.
4. Device A/B against RC6 measuring physical footprint, pipeline P95/P99,
   MSL import time, FPS, and thermal state. The device-teardown summary reports
   canonical publishes, dedupe hits, race losers, resident peak/evictions,
   resident-payload adoptions, rehydrates, and logical-membership peak.
5. Keep the Metal 4 compiler base-cache capacity and eviction policy identical
   across each A/B pair. Its compiled base pipelines are a separate bounded
   owner, and changing that policy simultaneously would make memory results
   ambiguous.


## Runtime memory and reclaim telemetry

The private diagnostic ABI exposes two nonblocking snapshots:

- `vkGetPipelineCacheMemoryStatisticsMVK` reports the exact compressed and
  uncompressed MSL represented by one logical `VkPipelineCache` view plus a
  lower-bound estimate of that view's C++ index storage.
- `vkGetMetal4ShaderLibraryRepositoryStatisticsMVK` reports the unique physical
  canonical payload, logical membership count, resident/cold counts, reclaim
  proxy bytes, and rehydrate latency.

Metal does not expose the allocated byte size of an individual `MTLLibrary`.
`residentUncompressedMSLBytes` and `evictedUncompressedMSLBytes` are explicitly
workload-size proxies. `deviceCurrentAllocatedBytes` and process `phys_footprint`
remain the evidence for actual memory release. Snapshot acquisition uses
`try_lock`; a busy cache or library is reported as unavailable/skipped rather
than blocking a render or compiler thread.
