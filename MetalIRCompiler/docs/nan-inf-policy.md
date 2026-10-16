# NaN and Inf in IR Safe and Relaxed

A Vulkan shader without `SignedZeroInfNanPreserve` or `FPFastMathDefault` lets
the implementation assume no NaN or Inf. Mesa's `spirv_to_nir` takes that
license: only kernels preserve NaN/Inf by default, and real-number rewrites
marked inexact (`~` in `nir_opt_algebraic.py`) apply to every float
instruction that is not `NoContraction`. Every TOTK module is such a legacy
module.

The MSL route compiles Safe without fast math and Relaxed without `nnan` or
`ninf`, so NaN and Inf survive. Before this change, IR Safe and Relaxed
diverged from it:

- `1 - clamp(NaN, 0, 1)` became `sat(1 - NaN)`. vtn drops NaN preservation for
  `FClamp`, so `fadd(fneg(fsat(nnan) a), 1.0) -> fsat(1 - a)` fired. This is v2b
  finding F1 (TOTK `5f4856021c8d4743…`, `case-0422-input-0`): MSL alpha is 1/30,
  IR alpha is 0.
- `isnan(x - x)` with `x = Inf` became false, because `~fadd(fneg a, a) -> 0`
  ignores NaN/Inf even when the instruction preserves them.

Patch 0013 adds a `preserve_nan_inf` runtime option to `spirv_to_dxil`. When it
is set, vtn marks every float instruction `preserve_nan | preserve_inf |
no_transform`, including `FClamp`. `no_transform` turns off the inexact
real-number rewrites. Contraction and reassociation keep their own bits.

`compiler_plugin.mm` sets the option for Safe and Relaxed, and leaves it off in
Fast. Relaxed still gets reassociation and contraction from the AIR adapter,
which marks binops without `nnan` or `ninf`, the same way as MSL Relaxed.

Patch 0006 used to select those Relaxed markers with `nir_alu_instr_is_exact`,
which counts `no_transform`. Under the new policy every float op would then
count as exact, and Relaxed would compile like Safe. Patch 0013 makes the
Relaxed branch of `emit_binop` check only `no_contract` and `no_reassoc`, so
`NoContraction` ops stay exact and all other binops keep `nsz | arcp`. The Fast
branch still uses `nir_alu_instr_is_exact`.

The math-mode policy, the frontend SPIR-V generator and the Mesa/MSC pins are
unchanged. Fast still assumes no NaN or Inf on both routes. Under the default
on-demand policy every legacy module resolves to Fast, so the production
default does not change.

## Oracle

`nan_default.spvasm` is a legacy compute module (no float controls). Its five
dynamic rows are:

- `1 - clamp(NaN, 0, 1)`
- `isnan(Inf - Inf)`
- `isinf(Inf * 2)`
- `isnan(Inf * 0)`
- `NaN == NaN`

Build `nan_default_probe` in `Tests/MetalIRReplay` and run it through the
canonical queue:

```sh
python3 /path/to/canonical/tool.py queue exec --label 'nan default' -- \
  python3 Tests/MetalIRReplay/run_nan_default.py \
  --build /private/replay-build --native /private/native/libMoltenVK.dylib \
  --plugin /private/compiler/libMeloNXMetalIR.dylib --output /private/new-results
```

The runner covers MSL and IR in Fast (on-demand) and Safe
(`MVK_CONFIG_FAST_MATH_ENABLED=0`). It checks the loaded native, the IR plugin
and the observed IR math mode, and it records identities before and after
execution.

Safe must preserve all five rows. Fast is reported but not required.
Relaxed shares the Safe code path in the plugin, but this legacy module cannot
select it under the on-demand policy, so the oracle does not run it.

## Evidence

The baseline is R01b: patches 0001–0012 (source `ff0dcb91`), Mesa
`7a1fc756809f3bdc9771b54e6036b156389dfc85`, MSC 3.1.1, and the frozen host
native.

| Run | MSL Safe | IR Safe | MSL Fast | IR Fast |
| --- | --- | --- | --- | --- |
| RED (R01b) | 5/5 | 3/5 (clamp, `Inf - Inf`) | 3/5 | 3/5 |
| `preserve_nan`/`preserve_inf` only | 5/5 | 4/5 (`Inf - Inf`) | 3/5 | 3/5 |
| GREEN (0013) | 5/5 | 5/5 | 3/5 | 3/5 |

- The intermediate row is a discarded variant without `no_transform`. In its
  static AIR the clamp order is fixed, but `Inf - Inf` is still stored as the
  constant 0.
- A second discarded variant set `no_transform` without the `emit_binop`
  change. It passed the oracle, but IR Relaxed lost its markers: across the
  8,593 IR Relaxed artifacts, binops with `contract` fell from 20,570 to 334.

The full static matrix (8,623 modules, all routes and modes) compared with
R01b:

- Changed artifacts: 2,354 in IR Safe and the same 2,354 in IR Relaxed. No
  Fast or MSL artifact changed, and no case changed classification.
- IR Relaxed keeps the same number of `contract` binops as the
  preserve-only variant in every module (20,570 in 4,046 modules).
- Static size on the changed artifacts: bytes grow 0.02% over R01b
  (62.29 MB to 62.30 MB in Safe). AIR instructions grow 0.10% over the
  preserve-only variant (5,149,758 to 5,154,985 in Safe). This is a size
  proxy, not a runtime measurement.

Other checks with the GREEN compiler:

- Official CTS slice (126 cases) in IR Safe and Relaxed: 116 pass, 8 fail,
  2 not supported, case-for-case identical to the C0 baseline run.
- `minmax`, `nocontraction_vector` and the strong FMA probes (f32/f16, plain
  and `--rules`) give the same results as R01b. `minmax` Safe still fails on
  both routes for signed zero, as before.
- v2b execution sweep: F1 (`case-0422-input-0`) and five more modules whose
  NaN outputs differed from MSL (`case-0554`, `0742`, `1175`, `2120`, `2170`,
  input 0) now match MSL element by element in Safe and Relaxed, with NaN
  compared as NaN. They still differ in Fast.

This is host compiler and host GPU evidence, not phone acceptance or a Vulkan
conformance claim.
