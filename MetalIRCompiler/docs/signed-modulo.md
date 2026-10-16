# Signed modulo in the Mesa-to-DXIL bridge

`nir_op_imod` was emitted as unsigned `urem`. For dynamic `a = -5, b = 3`,
`OpSMod` requires `1`; the old IR path returned `2`. Patch 0011 emits `srem`
and adds the divisor only for a nonzero remainder whose sign differs from the
divisor. `OpSRem`, unsigned modulo, the Mesa pin and the math-mode policy stay
unchanged.

The Vulkan negative-operand contract requires **maintenance8 to be enabled**.
The fixture queries and enables that feature explicitly. It excludes division
by zero and `INT_MIN / -1`, uses dynamic SSBO inputs, and checks both modulo and
remainder against a widened CPU reference. See the [Vulkan SPIR-V environment
rules](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).

## Reproduction

Build the `signed_modulo_probe` target in `Tests/MetalIRReplay` with the existing
`VULKAN_HEADERS` and `MOLTENVK_LIBRARY` CMake options. All GPU execution uses the
host's canonical graphics queue:

```sh
python3 /path/to/canonical/tool.py queue exec --label 'signed modulo' -- \
  python3 Tests/MetalIRReplay/run_signed_modulo.py \
  --build /private/replay-build --native /private/native/libMoltenVK.dylib \
  --plugin /private/compiler/libMeloNXMetalIR.dylib --output /private/new-results
```

The six cases cover MSL/IR × Fast/Safe/Relaxed. The runner verifies the actual
native library, IR plugin and observed IR math mode, retains errors without
fallback, and records source/binary identities before and after execution.

## Evidence

- Baseline source: `fcf2fb8e228628531e32fb438aef0454c2b9a644`, patches 0001–0010,
  Mesa `7a1fc756809f3bdc9771b54e6036b156389dfc85`, MSC 3.1.1.
- RED: all three MSL modes pass 20/20 rows. All three IR modes pass 6/20 modulo
  rows, while remainder and input integrity pass 20/20. Fourteen rows distinguish
  signed modulo from the former unsigned result. Pre-MSC DXIL contains `urem`.
- GREEN with 0011: both routes pass 20/20 in all three modes. DXIL now has signed
  remainder and sign correction, with no unsigned remainder for this fixture.
- An initial diagnostic run omitted maintenance8. It is retained privately as
  an invalid conformance fixture and is not the RED evidence above.
- The frozen 976-module host corpus contains zero `OpSMod` operations. Its
  generated artifacts are unchanged by 0011. This repair is not evidence of a
  particular game's visual defect being fixed.
- Pinned Vulkan CTS `bee83722547054754049485ae6ae8344496c02a6`: baseline IR
  fails `maintenance8.opsmod.all` and `maintenance8.opsmod64.all`; the six
  positive-input/remainder controls pass. With 0011, all eight pass in MSL and
  IR under each of the three verified diagnostic math modes.
- The existing weak-FMA matrix passes 18/18 and math-policy matrix passes 48/48
  with 0011 alone. The 126-case CTS slice has identical results in all six
  route/mode combinations: 116 pass, eight shared min/max signed-zero failures,
  two unsupported float-control cases. No IR-only failure in that slice.
- Fixed-frame replay is pending the plan's 20 GiB local free-space floor.
  No game or phone acceptance is claimed by this PR.

This is host compiler evidence, not phone acceptance or a Vulkan conformance
claim. Full regression and CTS outcomes are recorded in the accompanying audit
report; raw game-derived corpus data is private.
