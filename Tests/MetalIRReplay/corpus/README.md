# Offline shader-cache corpus

The extraction and statistics tools inspect a stopped read-only Ryujinx host-cache
snapshot without loading a graphics library. The optional compilation matrix
loads the compiler plugin and must use the shared graphics queue when it loads
Metal providers. None of these tools starts a game or a phone application.

`extract_cache.py` implements the TOCH v1.2 offset/length record format and the
stage packet in MeloNX `DiskCacheHostStorage`, `BinarySerializer`, and
`ShaderBinarySerializer` (format checked at `46996b2e`). It does not search for
SPIR-V magic. Compression values 0/1/2 are None/raw Deflate/Brotli; Brotli requires
the optional Python `brotli` package. Shared/guest metadata is not decoded, and
this is not a validator for the entire cache or its specialization state.

```sh
python3 extract_cache.py --toc /private/snapshot/vulkan_apple.toc \
  --data /private/snapshot/vulkan_apple.data \
  --output /private/new-corpus --validator /path/to/spirv-val \
  --source-label example-host --target-env vulkan1.2
python3 stats.py --manifest /private/new-corpus/manifest.json \
  --grammar /path/to/spirv.core.grammar.json \
  --glsl-grammar /path/to/extinst.glsl.std.450.grammar.json \
  --output /private/new-statistics
python3 -m unittest discover -s . -p 'test_*.py'
```

Use new output directories. Extraction returns 0 only for a complete, unchanged
input with all modules validated; 2 preserves an incomplete manifest. Invalid
TOC format fails before extraction. The 16 MiB per-program bound mirrors the
frontend. Output has a configurable total budget, stage/SHA identities and source
record references. Duplicate modules are exported once; counts in `stats.py` are
static instruction sites in unique modules, not runtime execution frequency.

Keep modules, manifests, statistics and logs outside the repository. The manifest
records TOCH's codegen field as stored (normally zero); the shared TOCS contains
the frontend host-codegen version. Do not identify the generating frontend or
claim a phone capture from this field alone. Record snapshot provenance separately.

The tests use synthetic packets, including corruption, duplicate stages, trailing
compression data and an unreferenced magic-value decoy. Actual corpus validation
is additional evidence, not a substitute for those format-boundary tests.

## B4 compilation matrix

Build the helper against the same pinned SPIRV-Cross headers and archives as
MoltenVK, with its namespace override. The helper itself does not link Metal.

```sh
cmake -S . -B /private/corpus-helper \
  -DSPIRV_CROSS_INCLUDE=/sdk/pinned-cross \
  -DSPIRV_CROSS_LIBDIR=/sdk/pinned-cross-build
cmake --build /private/corpus-helper
python3 /path/to/canonical/tool.py queue exec --label 'B4 corpus' -- \
  python3 compile_matrix.py --manifest /private/new-corpus/manifest.json \
  --helper /private/corpus-helper/compile_module \
  --guard /private/corpus-helper/libno_metal_guard.dylib \
  --plugin /private/compiler/libMeloNXMetalIR.dylib \
  --output /private/new-compilation --queued --workers 1
```

Use `--limit 3` in a new output directory for one module from each represented
stage. A positive control checks the provider-load guard before every run.
MSC 3.1.1 dynamically loads MPS and Metal despite having neither as a direct
dependency: this was caught by the guard, so its entire matrix is queued. The
queued path records those loads and runs sequentially. It does not claim that
device creation was instrumented. A forced stop uses canonical `queue cancel
<ticket> --stop`; the runner never kills queued children on a subprocess timeout.

Each validated module is compiled through both routes for all three requested
math modes. IR calls ABI 9 directly with a minimal reflected descriptor layout,
vertex invariance and zero transform flags. An active fragment `PointCoord`
selects the point-rasterization runtime flag, matching the native pipeline
variant required for defined point-coordinate consumption. This is not the original
pipeline layout; specialization-dependent/runtime descriptor arrays and aliases
are explicitly rejected as fixture limitations. MSL means **SPIRV-Cross source
generation only**: math mode is applied by later Metal compilation, which this
tool does not run. Equal MSL hashes across modes are consequently expected.

`failures.json` separates compiler failures, signals, unsupported fixtures and
guard errors. `differences.json` contains route-status and mode-artifact-hash
differences, not claimed numerical errors. There is no fallback. Identities of
inputs, helper, guard, sources, plugin and dependencies are checked before/after;
the child also verifies the actual loaded compiler paths. Retain the original
run if analysis or tooling changes later. Outputs contain private game-derived
data and must stay outside Git. The default artifact budget is 2 GiB.
