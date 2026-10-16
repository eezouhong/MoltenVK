# Strong FMA in the Mesa-to-DXIL bridge

`OpFmaKHR` (`SPV_KHR_fma`) requires one rounding. Mesa lowers it to
`nir_op_ffma`, which carries the module's float controls, and `nir_to_dxil`
emitted DXIL `Fma` (opcode 47) for every width.
DXIL `Fma` is defined only for doubles. MSC 3.1.1 aborts on its half/float
overloads (`patchTertiary: need to add support for opcode: 47`), so any IR
pipeline with a strong f16/f32 FMA failed to compile.

Patch 0012 emits DXIL `FMad` (46) for non-64-bit `ffma`, which MSC lowers to an
AIR `fma` intrinsic. The single rounding is kept in every math mode. Doubles
still use `Fma`. Weak FMA (patch 0010), the Mesa pin, the frontend SPIR-V
generator and the math-mode policy are unchanged.

## Oracles

All GPU work runs through the host's canonical graphics queue. Build the
`strong_fma_probe` and `strong_fma_rules_probe` targets in `Tests/MetalIRReplay`
with the existing `VULKAN_HEADERS` and `MOLTENVK_LIBRARY` options:

```sh
python3 /path/to/canonical/tool.py queue exec --label 'strong fma' -- \
  python3 Tests/MetalIRReplay/run_strong_fma.py \
  --build /private/replay-build --native /private/native/libMoltenVK.dylib \
  --plugin /private/compiler/libMeloNXMetalIR.dylib --output /private/new-results \
  [--half] [--rules]
```

The fixtures enable `VK_KHR_shader_fma` (`shaderFmaFloat32`, plus
`shaderFmaFloat16` and `shaderFloat16` for `--half`). Inputs are dynamic SSBO
values unless a row says otherwise.

- **Rounding sentinel** (`strong_fma*.spvasm`): four rows where the fused and
  split answers differ. It declares `FPFastMathDefault` with contraction,
  reassociation, NSZ and reciprocal allowed, so the on-demand policy really
  selects Relaxed.
- **Rules** (`strong_fma_rules*.spvasm`): `FPFastMathDefault None`, so every row
  has exactly one legal answer. The 12 rows are:
  - the dynamic fusion row
  - `fma(-0, 1, +0)` with a constant `+0` addend
  - `fma(-0, 1.0, +0)` with constant `1.0` and `+0`
  - the same with a dynamic `+0` addend
  - constant `b, c` operands
  - all-constant operands
  - negated sources
  - a chained FMA that turns a split first stage into `-1`
  - four vector lanes

  Each row also records the value that splitting the fusion or folding
  `fma(a, b, +0)` to `a*b` would produce. The probe refuses to run if a row
  cannot tell the two apart.

The runner verifies the loaded native, IR plugin and observed IR math mode, keeps
compiler errors without fallback, and records source and binary identities before
and after execution.

## Evidence

Baseline: R02 compiler (patches 0001–0011, source `ac948bb4`), Mesa
`7a1fc756809f3bdc9771b54e6036b156389dfc85`, MSC 3.1.1, frozen host native.

- RED (R02): MSL passes every row in Fast, Safe and Relaxed. In all three modes
  the IR route aborts in MSC with opcode 47 for f32 and f16, for both the
  rounding and the rules fixtures.
- GREEN (R02 + 0012): the rounding sentinel passes 4/4 in all six route/mode
  cases for f32 and f16. The Relaxed run really compiles in IR mode 2.
- GREEN rules: 12/12 rows in MSL and IR under Fast and Safe, for f32 and f16.
  - A Relaxed request for this strict shader is compiled in Safe (mode 1) by the
    on-demand policy. The runner therefore marks that sample as not proving
    Relaxed, even though its rows also pass.
  - Relaxed itself is covered only by the rounding sentinel.
- Full 8,623-module TOTK static matrix (51,738 compiler cases): R01b artifacts
  and statuses are identical to R02 for every case (51,645 compiled, 93 compiler
  failures in both). The corpus has no strong FMA. This is non-regression
  evidence, not a game trigger.
- Static AIR: the R02 IR route rejects all four fixtures in every mode, and R01b
  compiles them to `air.fma.f32`/`air.fma.f16` calls (see below).

## Remaining risk

MSC attaches fast-math flags to the `air.fma` calls it emits for `FMad`, in
every mode. A static inspection of the four fixtures' IR metallibs (R01b,
direct compiler calls with explicit modes) found:

| IR math mode | `air.fma` flags |
| --- | --- |
| Safe | `reassoc nsz arcp afn` |
| Relaxed | `reassoc nsz arcp afn` (metallib byte-identical to Safe) |
| Fast | `reassoc nsz arcp contract afn` |

The rules fixture declares no relaxations and DXIL calls carry no fast-math
flags, so the flags come from MSC. SPIRV-Cross output differs:

| MSL math mode | `air.fma` flags |
| --- | --- |
| Safe | None |
| Relaxed | `reassoc nsz arcp contract afn` |
| Fast | `fast` |

Ten of the twelve rules rows reach MSC as one `air.fma` call each, including
both constant-`+0`-addend rows, the constant-operand row, the negated row, the
outer stage of the chain and the four lanes. Mesa folds the remaining two,
`fma(-0, 1.0, +0)` and the all-constant row, before DXIL, so those rows test
NIR folding rather than the flags. The rows probe the rewrites those flags
would permit for one FMA:

- dropping a signed-zero addend
- folding constants
- splitting fusion

None was observed on this host GPU. That is evidence for the tested driver and
inputs, not a guarantee that a later Metal backend keeps the call exact.

The same MSC behavior applies to the `air.fast_*` intrinsics it emits for other
DXIL calls. That is a separate IR Safe vs MSL Safe difference, outside this patch.

This is host compiler evidence, not phone acceptance or a Vulkan conformance
claim.
