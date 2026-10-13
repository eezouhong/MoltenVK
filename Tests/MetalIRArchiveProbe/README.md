# Legacy IR PSO binary-archive evidence probe

This independent diagnostic uses only the legacy Metal render pipeline API and
`MTLBinaryArchive`. It does not link MoltenVK or the IR compiler, change the
product route, enable IR defaults, or call Metal 4 APIs. Frozen IR metallibs and
exact legacy descriptors are reused from task 3, outside Git.

## Build and isolation

Build only, without a graphics ticket, using a macOS 26+ SDK:

```sh
python3 build.py --build /owned/new-build \
  --source-manifest /owned/captured-partial-inputs.json \
  --developer-dir /Applications/Xcode-27-beta-3.app/Contents/Developer
```

The output is a uniquely identified, ad-hoc signed App Sandbox bundle. Its
Foundation-only `paths` helper does not create a Metal device. The builder stores
a fresh owner UUID in the signed bundle and private Darwin cache namespace. Never
replace an existing ownership marker. Captured shader bytes and manifests remain
local; the builder freezes stage hashes before copying them to its owned bundle.

Use the returned executable, `owned-cache.json`, and container `tmp` output root.
Every Metal execution requires the canonical host queue:

```sh
python3 /path/to/canonical/tool.py queue exec --label "IR archive smoke" -- \
  python3 run.py --probe /owned/build/IRPSOArchiveProbe.app/Contents/MacOS/ir-pso-archive-probe \
  --owner /owned/build/owned-cache.json --output /owned/container/tmp/smoke \
  --counts 16 --stage smoke
```

After smoke succeeds, run separate bounded `--stage timing` and
`--stage concurrency` tickets for counts `200 1000 2000`. A failure stops the
batch, retaining its configuration, stdout, stderr, result and cache deletion
manifest. The 180-second child alarm bounds an offline diagnostic, not a game
route. No runner stays live while analysis or approval waits.

Cleanup verifies signature, sandbox entitlement, bundle/executable identity,
UID and matching UUID markers; rejects symlinked paths; fsyncs a deletion manifest;
and removes only that disposable namespace's nested `com.apple.metal` and
`com.apple.gpuarchiver`. The latter was observed during the first archive capture
and must also be cleared for a cold archive-fill comparison. It never
clears the shared Metal database or the sandbox container. Returned child cache
namespace must match the ownership proof. Driver-memory coldness is unknown.

## Measurements

- `baseline`: restore the same libraries, create legacy render PSOs without an archive.
- `capture`: add each descriptor before PSO creation, create the PSO with the
  archive attached, then serialize twice to the same URL on a background QoS queue. Record add,
  PSO, flush, archive bytes, thread identity, and process footprint checkpoints.
- `lookup`: open the saved archive in a fresh process, restore libraries through
  `newLibraryWithData`, and require every PSO to pass `FailOnBinaryArchiveMiss`.
  Successful ordinary PSO creation alone would not prove an archive hit.
- `miss`: strict lookup on an uncaptured descriptor, followed by ordinary
  compilation; the negative fixture must produce an actual miss before fallback.
- `corrupt`: reject a deliberately invalid disposable archive and confirm
  ordinary pipeline creation remains usable.
- `concurrent-control` / `concurrent-flush`: populate a fresh archive without
  constructing PSOs, then release another thread to create 32 legacy PSOs outside
  the capture cohort. In the flush arm, serialize concurrently. Preserve absolute
  start/end times and actual overlap; a nonoverlapping run cannot answer blocking.

Timing runs include two cold-cache A/B pairs with the same frozen inputs, plus
ordinary and strict-archive requests with retained system caches. The no-archive
baseline and archived lookup both get their own cache clearance in the cleared
comparison. Keep all first and repeated samples; never treat a first request as
proof of full driver-cache coldness.

## Scope and gates

The source is an incomplete 512-request captured render prefix. Deduplication
produces 309 full descriptors. First 200 remain exact; larger 1000/2000 cohorts
include explicit synthetic write-mask extensions. They are distinct descriptors,
not duplicates padding a count, but are not a full game's PSO distribution.
There is no compute coverage, draw/dispatch, pixel oracle, NativeAOT/effective
game profile, IR512 lifetime validation, or >100 ms game-frame acceptance here.

Process footprint growth is not an intrinsic serializer allocation count. Disk
archive size is measured directly; API-owned resident binary bytes are unavailable.
`MTLBinaryArchive` serialization is not Metal 4 serializer flush: no compiler or
serializer rotation can be inferred from this legacy fixture.

Task 3 is No-Go and the user withdrew Metal 4. These files deliver old-API archive
feasibility evidence. Product implementation, old-iOS Phase 3 integration and
hot-game capture/flush remain unimplemented and require their own evidence gates.
