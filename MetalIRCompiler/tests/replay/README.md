# Synthetic runtime replay

These sources package the existing execution fixtures. Expected rows come from
Vulkan command arguments, independently of the MSL or IR compiler output. They
contain no captured game shaders or assets.

Coverage: direct/indexed draws, negative base vertex, nonzero first instance,
multi-draw IDs, GPU-written indirect arguments, triangle fan conversion, compute
dispatch/base/indirect, shader switching, disk restore, missing compiler rejection,
empty descriptor-set slots, and explicit per-device selection. The seventeen
cases do not cover all resource
formats, point sprites, multiview, or end-of-buffer reads; add those separately.

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
