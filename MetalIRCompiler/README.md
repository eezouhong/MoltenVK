# Experimental Metal IR compiler

SPIR-V → pinned Mesa `spirv_to_dxil` → Apple Metal Shader Converter → metallib.
MoltenVK loads this plugin through ABI 9 in `MVKMetalIRBridge.h`. Guest compilation
errors fail explicitly; this plugin does not invoke the MSL source compiler.
The AIR adapter preserves native point rasterizer I/O and exact memory attributes.
This directory packages the existing implementation; runtime optimizations remain
separate work.

ABI 9 adds absolute per-kind offsets for compact 24-byte descriptor entries.
Only the CBV/SRV/UAV/sampler kinds used by a binding reserve table space. Both
root tables use the same allocation base; compiler ranges carry the exact native
entry offsets. Native and compiler bundles must be rebuilt together.

The shared shader mathematical policy is explicit: Fast=0,
Safe=1, Relaxed=2. Partial Relaxed retains NaN/Inf behavior while allowing
reassociation, contraction, signed-zero simplification and reciprocals.
Mesa marks only non-exact floating binary operations with `nsz/arcp`; the AIR
permission adapter adds `reassoc/contract` to those operations. It does not add
`nnan`, `ninf`, approximate-function or legacy unsafe-algebra permissions.
Operation-level `NoContraction` remains in the original SPIR-V.

Patch 0010 keeps source fp32 `fma` fused. Upstream DXIL options split it into a
multiply and add, which Relaxed contraction may regroup into a different single
rounding than the guest's FFMA. It is emitted as DXIL `FMad` because MSC 3.1.1
rejects f32 `Fma`; MSC lowers `FMad` to `air.fma.f32`, the same fused operation
MSL's `fma()` uses. Separate source multiply/add operations are not fused.

The adapter edits fixed-width flag fields in the original MSC bitstream and
updates its checksum. It preserves the type graph, metadata and offsets, avoiding
LLVM 17's upgrade from legacy typed pointers to opaque pointers. Unknown flag
encodings, narrow fields, invalid ranges and checksum failures reject IR
compilation. Older ABI plugins and frameworks must be rebuilt together with Mesa;
the native loader rejects mismatches. The cache key includes this ABI, the full
math mode and the compiler binary/dependency identity.

The native key also includes the MoltenVK revision and actual loaded sidecar
hashes. It includes each stage's used bindings with their real offsets/registers;
unused trailing layout entries do not create another artifact. The resident
cache bounds construction objects independently of pipeline metadata. Known
deterministic MSC failures are cached; unknown/allocation failures remain retryable.

IR reads the same per-device `metal4SharedShaderLibraryResidentLimit` as MSL.
The common default is 512 artifacts. The native configuration fallback also
changes MSL consumers that do not supply a resident limit from unlimited to 512;
MeloNX already supplies 512 explicitly. IR bounded limits additionally use a
64 MiB serialized-metallib ceiling, which can bind before the count limit for
large libraries.
A shared resident limit of zero means unlimited count and serialized bytes.
`MELONX_METAL_IR_MEMORY_CACHE=0` remains a separate diagnostic switch that
disables IR retention; it is not the shared resident-limit value.

Run `python3 Scripts/test-metal-ir-resident-cache.py` from the repository root
for CPU-only count/byte, ownership, and concurrency regression checks. CI runs
the same script with assertions enabled; it does not enable IR or create a Metal
device.

Mesa supports `MeshEXT` modules (no task shaders or payloads) and emits
a DXIL mesh shader: `SetMeshOutputCounts`, `StoreVertexOutput`, and per-primitive
outputs (`SV_CullPrimitive`) in the primitive signature. `EmitIndices` takes a
primitive's three indices together, so Mesa merges the SPIR-V component stores
after CSE; a partial index write fails the compile. Mesh outputs stay in place,
so Y flip and the [-1, 1] to [0, 1] depth conversion apply to each position
store; a depth conversion needs z and w in one store. MSC converts the shader to
a Metal mesh function, and MoltenVK binds its root table to the mesh stage.
SPIR-V 1.4 entry points (all mesh modules) list every global; Mesa drops the
resources no instruction references, as it already does before 1.4, because
MoltenVK puts only referenced bindings in the root signature.

A fragment stage whose pipeline has no depth or stencil attachment loses its
FragDepth / FragStencilRefEXT output before IR compilation, matching the MSL
route's `enable_frag_depth_builtin` / `enable_frag_stencil_ref_builtin`; Metal
otherwise rejects the pipeline. Run
`python3 Scripts/test-metal-ir-fragment-outputs.py` (needs glslangValidator and
SPIRV-Tools on `PATH`) for the CPU-only rewrite checks.

`MELONX_METAL_IR_COMPILE_WORKERS` accepts 1–8 (default 2) for process-wide native
compiler admission. Native memory relief runs once after a quiet completed
batch. Disk-cache teardown telemetry reports dropped/failed writes. These are
mechanisms, not a claim that the measured footprint target has been met.

Host compiler diagnostics require `--replay-trace`; normal builds compile them
out. GPU workloads always enter the Debug Tool queue.

## Dependencies

- Xcode **27**, macOS arm64; select it with `DEVELOPER_DIR`. Deployment target 26.0.
- Mesa from the private repository in `mesa/MESA_REPOSITORY`, branch `melonx`,
  exact commit in `mesa/MESA_COMMIT`. Its first commit is the tree of
  `https://gitlab.freedesktop.org/has207/mesa.git` 7a1fc756 (which includes its
  bindless extension); each MeloNX change follows as one commit. Change Mesa by
  committing there and moving `MESA_COMMIT`. `--mesa-repository` accepts a local
  clone for offline builds.
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
Version 2 also includes `calibration`: an empty scope immediately before each
selected real scope, on the same thread, using the same clock/checkpoint path.
Its calls equal sampled calls, rather than all calls. Subtract the empty mean
from the work mean before scaling by call count. The control adds observer work
outside the timed region; retain off/on checks and small negative corrected
values rather than clamping them. Concurrent publication can briefly skew the
two cumulative tables, so compare sufficiently long completed windows.

Counters accumulate; subtract snapshots around completed work. Synchronous
local replay flushes its current encoder thread at snapshot. Other active
threads can lag by 255 calls and flush on exit. CPU and component wall times
overlap: do not add them. Calibrate empty scopes and compare sampling off/on;
clock reads can exceed the cost of a tiny binding operation. Raw sampled sums
are not whole-frame costs or FPS measurements.

CPU-only contract/calibration test, from the repository root:

```sh
clang++ -std=c++17 -O2 -pthread -DMVK_REPLAY_TRACE=1 -I MoltenVK/MoltenVK/GPUObjects \
  Tests/ReplayTrace/binding_trace_test.cpp -o "$TEST_OUTPUT"
"$TEST_OUTPUT"
```

## Source and distribution notices

The vendored AIR writer and ValueEnumerator retain their LLVM license headers
(Apache-2.0 WITH LLVM-exception). Their upstream URLs and original hashes are in
`src/air/upstream-identity.json`; include LLVM's LICENSE.TXT when distributing.
Mesa source retains its upstream notices; distribute its applicable notices.
Apple MSC is obtained separately under Apple's terms; distribution must include
its LICENSE and Acknowledgements. No game shaders, saves, or captures belong here.


## Raw indirect runtime parameters (ABI7)

Pinned Mesa patch0005 lowers draw bases and compute group counts through a
read-only raw SRV at register space28. Static runtime fields retain their CBV
space31 and established offsets; push constants remain at space30. Live-use
metadata decides whether each root parameter is present. The native encoder
binds indirect argument addresses directly and declares their buffers resident;
it never reads GPU-written arguments on the CPU or blits these fields. Direct
commands upload/reuse their8/12-byte values through the existing arena.

Rebuild Mesa and the compiler plugin together after this ABI change. ABI6
plugins/frameworks are incompatible. Shader/cache identities include the ABI
and dependency hashes. Source builds remain fresh-directory builds; local
incremental repair artifacts are not a substitute for release reproducibility.

## Mathematical-policy validation

The replay CMake target also builds `math_probe` and prepares synthetic float
control variants using glslang and SPIR-V Tools. `tests/replay/run_math.py` takes
the same explicit build/native/plugin/output arguments as the system replay.
It tests real per-device selection with ON_DEMAND, NEVER and ALWAYS, checks
separate precise results and permitted finite/NaN/Inf results, requires Metal
shader validation, and freezes source/input/binary hashes. Execute it through
the same canonical graphics queue wrapper. These scalar checks do not establish
whole-game visual, FPS or memory acceptance.

`tests/math_adapter_test.cpp` is a CPU-only structural bitstream test. Link it
with `src/air_math_adapter.cpp` and the pinned LLVM 17 core/bitreader/bitwriter
dependencies; compile without `NDEBUG`. It generates its own typed-pointer
fixture and checks that only eligible permission bits and the checksum change,
including idempotence, no-op, invalid/truncated data, unsupported encodings and
insufficient field widths. An optional input path repeats these checks on the
seven-operation synthetic MSC fixture. It neither creates a Metal device nor
proves GPU execution; the Vulkan scalar oracles provide that separate evidence.

Build and run the structural checks from the repository root:

```sh
python3 MetalIRCompiler/tests/run_math_adapter.py \
  --llvm-config "$LLVM_17_SDK/bin/llvm-config" \
  --zstd-library "$MACOS_STATIC_ZSTD" --output "$NEW_CPU_TEST_OUTPUT"
```

The public scalar matrix separately covers defaults without `AllowTransform`,
defaults allowing transforms, and a mixed shader with two `NoContraction`
operations. Its precision samples include float32 inputs where explicit FMA and
separate multiply/add differ; a CPU guard rejects an ineffective sentinel.

### Native absolute vertex and instance IDs

Patch 0007 adds a separate Metal-only Mesa entry point. Existing Mesa config
and metadata structs and converter entry points retain their ABI. Vertex and
instance IDs travel through hidden uint input semantics; the AIR adapter
restores native builtins and disables the corresponding public `VATT` fetch
attributes without moving record ranges. A record MSC emits without the fetch
flag (the ID is declared but its value is unused) is kept as is. Ordinary
vertex attributes remain unchanged. If two spare input slots are unavailable,
the prior runtime route is retained. BaseVertex, BaseInstance and DrawID still use their explicit
runtime inputs. This stays on SM 6.6.

The optional compiler requires the matched Mesa sidecar exporting the new
entry point. An older sidecar causes plugin loading to fail explicitly; it
does not silently select MSL. The native/plugin binding ABI remains 9.
