# Synthetic runtime replay

These sources package the existing execution fixtures. Expected rows come from
Vulkan command arguments, independently of the MSL or IR compiler output. They
contain no captured game shaders or assets.

Coverage: direct/indexed draws, negative base vertex, nonzero first instance,
multi-draw IDs, GPU-written indirect arguments, triangle fan conversion, compute
dispatch/base/indirect, shader switching, disk restore, missing compiler rejection,
empty descriptor-set slots, and explicit per-device selection. The twenty-three
cases do not cover all resource
formats, point sprites, or multiview. Exact end-of-buffer draw/dispatch arguments
and indirect triangle fans are included. Argument buffers are written by GPU
transfer commands before consumption.

Build only (no GPU execution), using the selected Xcode 27 developer directory:

```sh
cmake -S MetalIRCompiler/tests/replay -B "$REPLAY_BUILD" \
  -DVULKAN_HEADERS="$VULKAN_HEADERS_INCLUDE" \
  -DMOLTENVK_LIBRARY="$MOLTENVK_DYLIB"
cmake --build "$REPLAY_BUILD" --parallel 2
```

Use a fresh output directory. On the shared MeloNX host, run through the canonical
Debug Tool queue; do not start the replay directly:

```sh
python3 "$DEBUG_TOOL" queue exec --label "Metal IR system values" --priority 100 -- \
  python3 MetalIRCompiler/tests/replay/run.py --build "$REPLAY_BUILD" \
    --native "$MOLTENVK_DYLIB" --plugin "$METAL_IR_PLUGIN" --output "$REPLAY_RESULTS"
```

The runner freezes binary/input hashes, verifies the loaded native library and
compiler selection, retains raw logs, and fails on unexpected fallback/rejection.
Use the pinned MSC 3.1.1 plugin build; the supplied plugin directory's dylib hashes
are recorded. This is correctness/cache validation, not a timing benchmark.

`run_concurrent.py` creates 32 distinct compute pipelines on one VkDevice with
one or four workers, then dispatches every pipeline and checks its independent
output and untouched sentinel rows. Each configuration runs ABBA with unique
entry names and literals to avoid system-cache reuse. It reports all samples,
batch wall time and throughput; two samples per configuration are insufficient
to infer game FPS or a stable small performance difference. No new application
thread pool is introduced.

`run_points.py` checks programmable sizes 1/2/4/8, PointCoord colors, triangle
topology with a vertex shader that writes PointSize, and persistent restoration.
Coverage and coordinates follow the
[Vulkan point rasterization rules](https://docs.vulkan.org/spec/latest/chapters/primsrast.html#primsrast-points)
and are checked across all 256 pixels; MSL is also checked against that CPU
oracle. Metal validation is required. This catches AIR pointer-type loss during
point metadata rewriting and does not replace the game's loading-screen visual
check. Both runners use the same required path arguments and queue wrapper.

`descriptor_probe` adds a separate 40-row CPU oracle: cross-binding writes and
templates, zero-count binding gaps, copies between different array layouts,
copies to/from combined immutable samplers, standalone sampled images/samplers,
immutable-to-mutable sampler copy, and a swizzled image view. Repeat
addressing and clamp addressing produce different colors, so copying over an
immutable sampler cannot silently pass. The expectations follow the
[Vulkan descriptor-copy rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkCopyDescriptorSet.html)
and do not assume that another renderer is correct.

Run `run_descriptors.py` with the same required arguments as `run.py`, under the
same queue wrapper. It defaults to IR; `--route msl` is an optional independent
comparison and still fails if its output violates the CPU oracle. `--pool`
requires an actual Metal 4 view-pool assignment, while `--shader-validation`
requires the validation layer to report activation. Both can be combined.

`math_probe` and `run_math.py` cover the shared Safe/Relaxed/Fast policy with
ON_DEMAND, NEVER and ALWAYS, actual float-controls2 feature enablement, explicit
precise operations, and permitted NaN/Inf results. The generator validates all
synthetic SPIR-V variants. NaN/Inf is not an oracle for Fast, which permits
finite-value assumptions; those cases use finite inputs. Mixed precise
per-operation defaults intersect to Safe, matching the MSL policy. Every run
requires GPU validation and verifies the exact loaded native/plugin identity.
The runner has the same required path arguments as the other suites and must
run through the queue. Scalar math success does not prove complete mathematical
conformance or game performance.

The expanded 48-case scalar matrix distinguishes an explicit `NoTransform`
default from permitted transforms and keeps legacy `NoContraction` alongside
ordinary relaxed operations in a separate variant. Isolated precise results
must match the CPU's separated float32 multiply/add exactly; the fixture first
proves its sentinel differs from FMA. Cases that permit contraction use a finite
tolerance for the corresponding output. This does not turn a legal optimization
into a precision failure.

`run_msc_negative.py` sends the same structurally valid FP64 compute shader
through two separate shader modules and pipeline requests. The pinned MSC
3.1.1 must reject it with `IRErrorCodeFP64Usage` (19), and native telemetry must
show one compiler call and unchanged positive MSC time after the second
request. Both Vulkan requests fail explicitly with no MSL fallback. This is an
unsupported-feature rejection/cache test; it submits no dispatch and makes no
rendering or Vulkan-conformance claim. Use the same path arguments and queue
wrapper as the other runners.

`run_native_ids.py` checks 60 direct/indexed draws with changing vertex and
instance origins, including negative indexed vertex IDs. The ID-only vertex
shader writes the actual builtins into 960 rows; untouched rows remain
sentinels. MSL and IR must match the independent CPU oracle with Metal API/GPU
validation. IR must compile the stage without runtime-data/draw-parameters/
draw-bases flags. This does not replace the mixed BaseVertex/BaseInstance/
DrawID suite, and makes no whole-game CPU claim.
