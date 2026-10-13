# IR / Metal 4 Phase 0 probe

This independent diagnostic restores existing IR metallibs and compares legacy
render PSO creation with Metal 4 base plus specialization. Compute uses the
existing Metal 4 compute API; it has no render-style specialization phase.
Nothing here changes the product route or its defaults.

Build with a macOS 26+ SDK (build does not acquire a graphics ticket):

```sh
clang++ -std=c++17 -O2 -fobjc-arc -fblocks -mmacosx-version-min=26.0 \
  -framework Foundation -framework Metal probe.mm -o /owned/build/ir-metal4-probe
```

`run.py` requires the canonical queue lease. Use the host's canonical Debug Tool:

```sh
python3 /path/to/canonical/tool.py queue exec --label "IR Metal4 Phase0" -- \
  python3 run.py --probe /owned/build/ir-metal4-probe \
    --inputs /owned/inputs.json --output /owned/new-results --variants 16
```

The input array contains `label`, absolute `vertex`, `fragment`, optional
`compute` metallib paths and matching `vertexSHA256`, `fragmentSHA256`, and
`computeSHA256`. Optional `vertexEntry`, `fragmentEntry`, and `computeEntry`
select function names; otherwise each library's first function is used.
Captured shaders and input manifests remain outside Git.

The runner executes ABAB in separate processes. Each process measures first and
repeated requests, library loading, render base/specialization/total nanoseconds,
compute total nanoseconds, errors, and process footprint. It verifies immutable
input/binary hashes before and after the batch and retains stdout/stderr. A
180-second child alarm bounds this small diagnostic; completion exits all owned
probe processes. Failed pipeline creation invalidates the comparison denominator.

Important limits:

- The vertex layout is inferred from stage attributes with synthetic offsets.
  Sixteen diagnostic color-format/blend/write-mask variants deliberately share
  one base per input. They are not the captured game's actual PSO distribution.
- This probe uses synchronous APIs. Product asynchronous-service overhead and
  cache admission/coalescing are not measured.
- It submits no draw/dispatch. Successful PSO creation does not establish pixel
  equality, GPU cost, or game acceptance.
- Default runs retain system caches, with initial coldness unknown. A first
  request in a process is not a system-cold request. ABAB can share compiled
  results across APIs.
- `cache_paths.mm` reads Foundation/Bundle filesystem metadata without a Metal
  API or GPU. A dedicated App Sandbox bundle resolves a private Darwin cache
  namespace. The signed owner UUID and namespace marker must agree.
- `--owned-cache-metadata FILE --system-cache clear-owned` verifies signing,
  sandbox entitlement, bundle/UUID/UID and namespace, rejects symlinks,
  persists the file deletion manifest, then removes only this disposable
  sandbox's nested `com.apple.metal` before each A/B subprocess. It verifies
  the actual child-reported namespace. The shared Metal database and container
  data are preserved. Driver-memory coldness is not assumed.
- `--cache-observation-root` performs metadata-only observation. Cache-file
  changes alone are not ownership proof.

`analyze_logs.py` summarizes existing Perf logs and reports missing per-PSO
identity/state as unavailable, not zero reuse. `extract_capture.py` streams only
creation records from an existing GFXReconstruct capture using its converter;
this is a CPU-only parse, with no replay. `analyze_capture.py` gives conservative
Vulkan-state and native-like projected group counts. Neither projection is the
actual `makeMetal4PipelineBaseKey`: IR artifact identity, used Metal attributes,
and all effective native fixed-state settings must be measured before claiming
native reuse. No synthetic state histogram is substituted for a title's workload.

Phase 1 remains gated on P0 completion, a cache-controlled representative PSO
comparison reducing total time by more than 30%, and task 2's correctness gate.
Serializer/archive capture, flush, loading and partitioning belong to task 4.

## Exact native input capture

`MVK_IR_PHASE0_PROBE=ON` is a separate CMake option, OFF by default. Its hooks
only preserve the IR request key, dump metallib bytes, and count the actual
`makeMetal4PipelineBaseKey` string on the legacy IR path. They install no
Metal 4 function descriptor and never activate a new IR PSO route. Set
`MELONX_IR_PHASE0_DUMP` to a new owned directory in this diagnostic build.
Captured render/compute descriptors and shaders remain outside Git.

`capture_native.py` runs one existing capture under the canonical queue with
loaded native/compiler SHA and source-diff proof, retains errors, and bounds
the owned replay to 180s. Capture IO invalidates its timing. The optional
`--remove-unsupported` normalization is diagnostic only; it cannot establish
unmodified-capture, game, or pixel acceptance.

`captured_probe.mm` consumes the exact Metal descriptors/keys. A/B retains the
same cohort libraries, with a 1024-entry LRU base cache. Neither policy
simulates product IR512 residency or service cost-based admission. It retains
failures instead of dropping inputs and submits no draw/dispatch. Timing still
needs the game's correctness gate and full product Debug Tool acceptance.

Pass the replay's `result.json` to `make_manifest.py --capture-result` so its
summary preserves source failures and marks incomplete coverage. A prefix from
an interrupted replay can diagnose API compatibility and reuse, but its timing
does not establish the complete capture's performance or pixel gate. Without
a verified source result, `sourceCaptureComplete` stays false.
