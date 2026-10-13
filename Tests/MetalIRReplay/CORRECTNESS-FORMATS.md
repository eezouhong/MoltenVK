# Attachment correctness gaps

The additive `correctness_formats_probe` target tests eight independent cases
under MSL and IR: UNORM stores, RB write mask, zero mask, separate RGB/alpha
blending, sRGB stores/blending, UINT stores and UINT RB mask. Every case checks
all 256 pixels and 16 untouched readback guard bytes. The integer oracle is exact;
normalized/encoded colors allow one byte for quantization. The CPU oracle uses
constant shader values and explicit clear/blend/mask arguments, never the output
of either compiler. The full-screen triangle avoids edge coverage ambiguity.

The sRGB oracle encodes RGB on clear/store and decodes the quantized destination
before blending; alpha stays linear. These expectations follow the Vulkan
[clear-value conversion](https://docs.vulkan.org/spec/latest/chapters/clears.html)
and [framebuffer blending](https://docs.vulkan.org/spec/latest/chapters/framebuffer.html)
rules. No blending is requested for UINT formats. Unsupported format capability
fails explicitly and does not count as a passed oracle.

Build the target using the replay's existing CMake options, without running it:

```sh
cmake --build "$REPLAY_BUILD" --target correctness_formats_probe --parallel 2
```

Run all cases through the shared queue, using a fresh output directory:

```sh
python3 "$CANONICAL_DEBUG_TOOL" queue exec --label "IR attachment correctness" -- \
  python3 Tests/MetalIRReplay/run_correctness_formats.py \
  --build "$REPLAY_BUILD" --native "$MOLTENVK_LIBRARY" \
  --plugin "$METAL_IR_PLUGIN" --output "$NEW_RESULTS"
```

The runner records source, shader, executable, native and compiler dependency
hashes, verifies actual loaded native and compiler route, requires API/GPU
validation, and preserves each failure while continuing to the other route.
It does not change production behavior or existing replay helpers.
Depth/stencil/MSAA, gather/comparison/offset sampling, clip/cull and subgroups
remain outside this fixture; do not describe it as full Vulkan conformance.
