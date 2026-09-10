# RC6 shared shader-library creation: short cache-view locking

Target branch: `eezouhong/MoltenVK:metal4/1.4.2-ryujinx.6`.
Baseline: `f74cdd53ec28a196a0ca8ae19849c4ace7d81d19`.

## Defect

`MVKPipelineCache::getShaderLibrary` held a pipeline-cache-wide mutex while a
missing library was converted and compiled. A second worker could not even
look up a *ready* library in the same cache until that compilation finished.
Separately, different logical caches could build the same shared-library input
before either published its result.

## Repair and ownership

Only the shared-repository path changes; repository-disabled users retain the
existing path. No public/private configuration ABI, cache serialization format,
Metal compiler slot limit, resident budget or application scheduling changes.

1. Look up the logical view and canonical repository under the existing short
   cache-view lock. This lookup is explicitly forbidden to compile.
2. On a miss, release that lock before entering a repository-owned creation gate.
   Recheck the view/canonical result after acquiring the gate.
3. Build in an unpublished, stack-owned `MVKShaderLibraryCache`. A matching
   deferred import is copied into this temporary view, not removed from the
   caller's serializable view. Conversion and Metal library compilation do not
   hold the shared cache-view mutex.
4. Adopt the canonical membership under the view lock and mark the view dirty.
   The temporary view holds a real repository reference until the destination
   has its own membership; its destructor releases on return or exception.
5. Merge uses retained logical snapshots: hold the source view lock to capture,
   release it, then hold the destination lock to publish. Never hold two cache
   view locks simultaneously or enter a creation gate under a view lock.

The **creation gate key is a module synchronization domain, not a library
identity**. Result lookup still uses the existing complete conversion-config
matching/alignment rules. Misses for different configurations of the same module
are intentionally serialized in this first repair; hits bypass the gate and
unrelated modules progress independently. Pre-conversion usage masks must not
be treated as proven full-key equality. This conservative scope avoids a
second, incorrect identity system and preserves same-module converter safety.

The weak gate registry holds neither library results nor owners alive. Owners
and waiters share the live gate; idle entries are pruned in batches. Failed
creation does not publish a failure memo or remove deferred data. A later
request can retry. `FAIL_ON_PIPELINE_COMPILE_REQUIRED` does not enter a creation
gate or start a missing compile. Existing resident/macro specialization checks
remain authoritative after library selection.

Everything here is synchronous: no cache/shader/device pointer escapes into
new background work. Vulkan's valid lifetime requirements still apply: the
caller must finish pending creation calls before destroying their cache/module,
and finish device operations before device destruction. This is not support
for an invalid concurrent `vkDestroyPipelineCache`.

## Tests delivered with the fix

### Controlled production control flow

`Scripts/test-shared-library-concurrency.py` compiles the **actual production
wrapper, new concurrent body and creation-gate header**, with deterministic
signals and dependency doubles for library construction and logical views.
It is not an alternate handwritten implementation of the scheduler. Its
configuration/compilation doubles are not a substitute for real Vulkan tests.

```sh
python3 Scripts/test-shared-library-concurrency.py --baseline f74cdd53
python3 Scripts/test-shared-library-concurrency.py --sanitizer thread
python3 Scripts/test-shared-library-concurrency.py --sanitizer address
python3 Scripts/test-shared-library-concurrency.py --sanitizer undefined
python3 Scripts/test-metal4-shared-shader-repository.py
```

The baseline's actual wrapper fails six of ten cases. The candidate passes all
ten under each of ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer:
independent-module hit/miss progress, same-module ready-hit progress,
compile-required early return, same-input cross-view coalescing, distinct config
identity, failed deferred-import retry, export while a miss is paused, exception
cleanup, and view/gate churn. The original shared-repository policy suite passes
without deleting or weakening its ownership assertions.

### Real Vulkan / Metal integration

```sh
python3 Scripts/test-shared-library-vulkan.py \
  --library /path/to/libMoltenVK.dylib \
  --headers /path/to/Vulkan-Headers/include \
  --glslc /path/to/glslc \
  --output /path/to/result.json
```

The harness builds four real SPIR-V modules, creates sixteen pipelines
concurrently across two caches, exports and merges a source cache while
creation is active, round-trips serialized data, and creates four more pipelines.
It destroys the original source cache only after its users finish, **before
executing the pipelines**, then verifies all **80 GPU-written words**.

Candidate runs passed with repository enabled/limit 0, disabled, and enabled/
limit 1. The positive-limit run exercises that configuration but is not claimed
to force resident eviction (the repository has a high-water/protection policy).
The enabled Metal4 run reports four successful library tasks for the four
modules. The same native harness also passes on the baseline; concurrent
baseline duplicate compilation is timing-dependent, so these native totals
are not advertised as a guaranteed task-count or FPS improvement.

The first harness invocation requested a loader-only extension not advertised
by direct MoltenVK and failed before creating a device. The harness now queries
available instance extensions. That failed attempt is retained separately.
An initial private-API build disabled Metal4 by existing policy; it is recorded
as legacy compatibility, not Metal4 validation. Primary runs use private APIs
OFF and confirm the actual Metal4 compiler in the runtime summary.

## Build and acceptance boundaries

The full macOS arm64 CMake Release native library builds successfully, using
pinned dependency sources and identical baseline/candidate build options.
Both changed Objective-C++ translation units also compile for **iPhoneOS arm64**
with the iOS SDK. The existing CMake iOS configuration stops at its AppKit lookup;
translation-unit compilation is not a linked iOS package or device test. No
phone is installed, operated or benchmarked by this repair.

A concentrated game check against the new native revision is recorded
separately once completed. Native controlled tests establish removal of the
specific lock dependency; they do not establish the cause of all prior
59-second versus 6-second Metal library task totals, or resolve the phone's
14/24 FPS split. Apple compiler/driver cache and host state remain separate
variables; no driver/user cache is cleared.
