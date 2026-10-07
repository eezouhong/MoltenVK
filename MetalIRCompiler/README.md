# Experimental Metal IR compiler

SPIR-V → pinned Mesa `spirv_to_dxil` → Apple Metal Shader Converter → metallib.
MoltenVK loads this plugin through ABI 6 in `MVKMetalIRBridge.h`. Guest compilation
errors fail explicitly; this plugin does not invoke the MSL source compiler.
The AIR adapter preserves native point rasterizer I/O and exact memory attributes.
This directory packages the existing implementation; runtime optimizations remain
separate work.

## Dependencies

- Xcode **27**, macOS arm64; select it with `DEVELOPER_DIR`. Deployment target 26.0.
- Mesa fork `https://gitlab.freedesktop.org/has207/mesa.git`, exact commit in
  `mesa/MESA_COMMIT`. Ordered patches in `mesa/patches/` reproduce the local changes.
  The pinned fork already includes its bindless extension.
- LLVM **17.0.6**, static SDK with `llvm-config`, headers, and core/bitreader/bitwriter
  dependencies. This is a dependency SDK; its standard archives are not rebuilt by
  the host script. The custom AIR writer and adapter are always rebuilt from source.
  Use a static macOS zstd archive if the SDK reports `-lzstd`.
- MSC **3.1.1**, exactly the Apple-signed macOS/iOS libraries and headers pinned by
  XeniOS at `2bbd3665b0143c1f18043ddd9a20de77ba65e834`. `msc/VERSION` records both
  library paths, download URLs and SHA-256 values. Supply the SDK root containing
  `include/`, `lib/`, and `lib_iOS/`; no Apple binaries belong in this repository.
- Python 3, Meson, Ninja, Git, pkg-config, Python Mako, PyYAML and packaging.
  Use an external virtual environment if these are not installed.

## Fresh host build

Start with a new, empty output directory outside this checkout. All source and
dependency paths are arguments. For example, with dependency locations selected
by the caller:

```sh
DEVELOPER_DIR="$XCODE_27_DEVELOPER_DIR" ./build-host.sh \
  --output "$BUILD_OUTPUT" \
  --msc-dir "$MSC_SDK" \
  --llvm-config "$LLVM_17_SDK/bin/llvm-config" \
  --zstd-library "$MACOS_STATIC_ZSTD" \
  --meson "$TOOLS_VENV/bin/meson" --jobs 4
```

An optional `--mesa-repository "$MESA_SOURCE_CACHE"` fetches the exact pinned
commit from a local Git source cache. It ignores that cache's working tree edits;
all patches are applied to a new checkout in the output directory. No existing
Mesa, plugin, or AIR object outputs are reused.

Outputs are `lib/libMeloNXMetalIR.dylib`, `lib/libspirv_to_dxil.dylib`, and a copy
of MSC, plus `identity.json` with dependency/source/binary hashes. The plugin uses
`@loader_path` to load its adjacent libraries. Set `MELONX_METAL_IR_PLUGIN` to the
new plugin path for validation. Builds do not execute GPU workloads.

## iOS

Device loading and performance are not yet validated. The first part of
`build-ios.sh` builds dependencies only, independently of MSC version selection:

```sh
DEVELOPER_DIR="$XCODE_27_DEVELOPER_DIR" ./build-ios.sh --dependencies-only \
  --output "$IOS_BUILD_OUTPUT" --llvm-source "$LLVM_SOURCE_ROOT" \
  --meson "$TOOLS_VENV/bin/meson" --jobs 4
```

`LLVM_SOURCE_ROOT` contains sibling `llvm/` and `cmake/` directories extracted from
the official 17.0.6 archives recorded in `llvm/SOURCE.json`. This builds a host
LLVM 17 tablegen first, then iOS 17+ arm64 core/bitreader/bitwriter and their
dependencies, with zstd/zlib/terminfo/libxml2 disabled. Mesa uses the same pin and
patches as the host, compiled with the iPhoneOS SDK. A local Mesa source cache can
be supplied as `--mesa-repository`, exactly as in the host build.

This mode does not produce a usable plugin or install/run a phone app. Framework
linking uses the iOS MSC slice; the macOS slice must never be embedded in a phone
app. Device installation needs the user's authorization.

Link completed dependency builds into the two framework slices:

```sh
DEVELOPER_DIR="$XCODE_27_DEVELOPER_DIR" ./build-ios.sh --link-frameworks \
  --output "$FRAMEWORK_OUTPUT" \
  --dependencies "$IOS_BUILD_OUTPUT/dependency-identity.json" \
  --host-build "$BUILD_OUTPUT" --msc-dir "$MSC_SDK"
```

The device framework statically links Mesa and LLVM and dynamically links the
pinned iOS MSC. `MeloNXMetalIR.xcframework` contains arm64 iOS and macOS slices;
`ios-runtime/libmetalirconverter.dylib` is copied unchanged from the signed input.
Embed & Sign that dylib in the app's Frameworks directory, as XeniOS does, along
with the selected compiler slice. Only the four compiler ABI functions are
exported. Framework linking does not establish device loading or performance.

`tests/prepare_nan_inf.py --output "$NEW_TEST_DIRECTORY"` prepares a synthetic
SPIR-V oracle from `tests/nan_inf.comp`, with explicit `SignedZeroInfNanPreserve`
float32 execution mode and `NoContraction` operations. A Vulkan 1.2 buffer gate
must check `shaderSignedZeroInfNanPreserveFloat32` before executing it. Each lane
returns `i*3+7`; failures add a bit mask times 65536. Without the preserve mode,
Vulkan permits finite-value assumptions, so that variant is not a valid strict
NaN/Inf oracle. No captured game shaders are included here. Execute this test
only through the shared graphics queue.

## Local binding attribution

`MELONX_REPLAY_BINDING_SAMPLING=1` enables diagnostic-only Bernoulli sampling
(1/128 calls). It changes no binding or rendering policy and emits no per-draw
log lines. `vkGetReplayBindingStatisticsMVK` returns eight `BindingSample`
records: MSL/IR pairs for resources, direct-draw preparation, Metal draw calls,
and graphics residency submission. Each record contains exact observed calls,
sampled calls, raw inclusive wall/CPU nanoseconds, unavailable samples, and three
wall-time components. Resource components are preparation, descriptor script,
and remaining binding. Other groups put the observed operation in component 1.
In coarse tracing mode the same records are emitted as `MELONX_BINDING_TOTALS`
at the existing one-second batch boundary. The JSON array uses the same group
order and field order as the struct; counters are cumulative, not per-frame.

Counters accumulate; subtract snapshots around completed work. Synchronous
local replay flushes its current encoder thread at snapshot. Other active
threads can lag by 255 calls and flush on exit. CPU and component wall times
overlap: do not add them. Calibrate empty scopes and compare sampling off/on;
clock reads can exceed the cost of a tiny binding operation. Raw sampled sums
are not whole-frame costs or FPS measurements.

CPU-only contract/calibration test, from the repository root:

```sh
clang++ -std=c++17 -O2 -pthread -I MoltenVK/MoltenVK/GPUObjects \
  MetalIRCompiler/tests/binding_trace_test.cpp -o "$TEST_OUTPUT"
"$TEST_OUTPUT"
```

## Source and distribution notices

The vendored AIR writer and ValueEnumerator retain their LLVM license headers
(Apache-2.0 WITH LLVM-exception). Their upstream URLs and original hashes are in
`src/air/upstream-identity.json`; include LLVM's LICENSE.TXT when distributing.
Mesa source retains its upstream notices; distribute its applicable notices.
Apple MSC is obtained separately under Apple's terms; distribution must include
its LICENSE and Acknowledgements. No game shaders, saves, or captures belong here.
