# IR compilation rule audit: A0 through C0 follow-up

Date: 2026-10-15/16. This combines the original source audit with the approved
B4, numerical-oracle and first CTS slice follow-up. It is **not a Vulkan
conformance certificate**. GPU work used the shared queue; no phone lifecycle or
release action occurred. The production repair is isolated in [PR #50](https://github.com/eezouhong/MoltenVK/pull/50).

The rejected shared-SPIR-V workaround, MeloNX #417, is closed. Keep Ryujinx output
unchanged. Further behavioral repairs belong in MetalIRCompiler and require a
failing independent oracle before the repair, followed by the same passing oracle.

## Frozen inputs and scope

| Input | Identity |
| --- | --- |
| MoltenVK / MetalIRCompiler | `fcf2fb8e228628531e32fb438aef0454c2b9a644` (merged #48) |
| Mesa fork, unchanged | `https://gitlab.freedesktop.org/has207/mesa.git`, `7a1fc756809f3bdc9771b54e6036b156389dfc85` |
| Existing Mesa patches | 0001 through 0010; hashes agree with the existing fresh-build identity |
| Common upstream ancestor | `0c195a01b06d693274b51af58d921071643fb5a7` (2026-07-01 UTC) |
| Frontend, read only | MeloNX `46996b2e5cec306128686160f765a7f00998276f`; inspected cache format and SPIR-V generator are unchanged from `65ed3e2c` |
| SPIRV-Cross | `6c09849fe88c48eaed08413aa022aaa136a3a057`, matching `ExternalRevisions/SPIRV-Cross_repo_revision` |
| Installed converter | MSC 3.1.1; macOS dylib SHA-256 `af26d2aabe35c74d80d5701d5a42d845ae3a776822cc3c87c0703472a71a7752` |
| LLVM | 17.0.6, retained dependency |
| SPIR-V validator | SPIRV-Tools v2026.3; `--target-env vulkan1.2` |

The patched Mesa snapshot was inspected without editing it. Its nine-file diff
has SHA-256 `2968e70b2d23b57585c5b34d4d90ad5a65bf14953f0335358db9811d033bb9a0`.
No official Mesa build was attempted, and neither `MESA_COMMIT` nor the fork URL
was changed. Official KosmicKrisp source is a read-only comparison input only.

**Vocabulary:** `MATCH (source)` means the inspected branch or representation is
consistent with the stated obligation, not that an end-to-end GPU oracle passed.
`RISK` includes unknown behavior and missing coverage; it does not mean a proven
bug. `INACTIVE option` means that particular option is zero, not that another
explicit pass cannot lower the same operation. `PR48 evidence` refers to earlier
recorded tests, not a new run in this audit.

## A0: all eleven fork commits

Each complete diff was read, in addition to its commit message.

| Commit | Change | Actual dependency / audit disposition |
| --- | --- | --- |
| `756b5168` | Build full NIR for spirv-to-dxil-only configurations | Required by the current minimal Mesa build. Build selection, not numerical semantics. |
| `441ed609` | Synthesize SV_Barycentrics signatures and read them | Conditional path. Pixel/centroid/sample forms share a synthesized signature; test interpolation mode and sample position before claiming equivalence (R12/R13). |
| `3e522f15` | Per-vertex fragment input uses attributeAtVertex | Conditional path. The DXIL vertex index is immediate; prove preceding indirect-access lowering leaves a constant for valid dynamic source indexing (R13). |
| `ea869799` | Opaque-resource-only bindless lowering | Present but the production plugin sets `lower_to_bindless=false`. No claim that this inactive path was GPU-tested. |
| `1d53c869` | Public bindless option and optional identity remapper | The configuration field is an API dependency; **the lowering itself is disabled**. It would be inaccurate to list active bindless lowering as a current dependency. |
| `67e28b19` | Multi-stage linking entry point | Present but unused by `MeloNXCompileMetalIR`, which invokes the single-stage absolute-ID entry point. Keep isolated-stage I/O checks (R13). |
| `38db1ff6` | `keep_io_vars` | Active, true. Preserves unused inter-stage declarations; interacts with patch 0009's Vulkan location preservation. |
| `1a8e6f2c` | Fragment interlock to ROV resource metadata | Conditional. Begin/end disappear after a linear region scan; unresolved resource identity marks all UAVs. Ordering, branches, aliases, and sample/pixel scope remain RISK (R13), not proven by the comment's "always correct" assertion. |
| `a0e2a9c2` | Plumb previous-stage clip count | API retained; current plugin zero-initializes `input_clip_size`. Check independent-stage clip/cull classification with declared nonzero counts (R13). |
| `7f922209` | Respect disable-math-refactoring on binary ops | Active foundation. See the exact interaction below; it is not a global NIR optimization switch. |
| `7a1fc756` | Portable `c99_alloca.h` | Required macOS build portability; no shader arithmetic rewrite. |

### `7f922209` + 0006 + 0010

`7f922209` stops setting `DXIL_UNSAFE_ALGEBRA` on non-exact binary operations when
`disable_math_refactoring` is true. It also forwards that setting through both
Mesa entry points. The module flag was already present.

Patch 0006 deliberately gives Relaxed non-exact binary operations `NSZ|ARCP`
while leaving the module disable-refactoring flag set. The AIR adapter later
adds `REASSOC|CONTRACT` to that exact marker. Safe emits neither marker nor unsafe
binary flags. Fast emits the legacy unsafe-algebra flag. An NIR instruction with
any `nir_fp_exact` bit takes the flag-free binary path in all three modes.

Patch 0010 changes the **mode-independent NIR option** `float_mul_add32` to
`keep_weak_ffma` (0x08, without the fuse bit). Weak f32 FMA is retained and emitted
as DXIL FMad (46). Strong NIR FMA still selects DXIL Fma (47). The latter distinction
is central to R01. These patches are complementary; none disables all NIR rewrites.

## Correct oracle contracts

Two result classes must remain separate:

1. **Vulkan conformance:** use the supported feature set, explicit float controls,
   instruction-specific result allowance, and defined input domain.
2. **Guest/MSL compatibility:** a tighter product requirement can require the
   guest's fused rounding even where Vulkan permits more than one result.

GLSL.std.450 `Fma` does not guarantee a hardware-fused single rounding. `OpFmaKHR`
provides the strong operation. Therefore the existing `fused_fma` fixture's
single-rounding assertions are useful compatibility tests; its comment describing
that as a universal GLSL Fma requirement is too strong. Keep its evidence, but add
an independent strong-FMA conformance fixture instead of relabeling the old test.
[Khronos FMA rationale](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_shader_fma.html).

Do not fail an implementation merely for a permitted NaN selection in FMin/FMax,
allowed rounding error, or different results for undefined/poison inputs. In
particular, distinguish FMin from NMin, Round from RoundEven, ordinary float-to-int
conversion from an explicit saturating operation, and valid shifts from counts at
or beyond the operand width. `INT_MIN/-1`, division by zero, and out-of-range
conversions need separately classified diagnostic rows, not fabricated IEEE
expected values. MSL is a valuable control, not the normative definition for all
of these cases. See [SPIR-V](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html),
[GLSL.std.450](https://registry.khronos.org/SPIR-V/specs/unified1/GLSL.std.450.html), and
[Vulkan floating-point rules](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html).

## Offline workload evidence (B1–B3 prerequisite for A)

The new `Tests/MetalIRReplay/corpus/extract_cache.py` follows TOCH v1.2:
32-byte header, 16-byte offset/uncompressed-size/compressed-size entries, compression
byte, then the count/stage/length packet used by `ShaderBinarySerializer`. It does
not scan for magic values. Empty and invalidated references are recorded; bounded
records, packet tails, stage identities, and every SPIR-V module are checked.
`stats.py` hashes its inputs and counts opcodes, extended instructions, decorations,
and execution modes. Nine synthetic parser/statistics tests passed, including an
unreferenced SPIR-V decoy that must not be extracted.

The available stopped **host** control cache was copied read-only and checked
before/after. It is not a phone-generated cache. No phone-cache snapshot was present
in the previously collected phone evidence searched for this task; broader phone
and multi-title coverage remains B-stage work. The TOCH codegen field is zero by
format design; the shared TOCS holds the frontend codegen version.

- 683 program records, 1,340 stage references, **976 unique modules** (432 vertex,
  518 fragment, 26 compute); 27,586,004 module bytes.
- Every unique module passed `spirv-val`; zero extraction errors.
- Fma: 54,088 sites / 893 modules; FMul: 49,318 / 936; FAdd: 23,498 / 857.
- FClamp: 4,588 / 538; FMax: 2,800 / 532; Trunc: 2,359 / 435;
  InverseSqrt: 1,836 / 605; Sqrt: 1,252 / 426.
- NoContraction: 117 decorations / 65 modules; Invariant: 432 / 432.
- RelaxedPrecision, FPFastMathMode/Default, DenormPreserve/FlushToZero,
  SignedZeroInfNanPreserve and RTE/RTZ execution modes: **zero in this sample**.

These are static-site/module counts, not draw frequency or performance hotness.
Raw modules, manifests, source mappings, validation logs, and full statistics are
private. Zero observations do not establish support for an unexercised instruction.
B4 now has a complete three-mode IR/SPIRV-Cross result; see the follow-up below.
MSL here means source generation, not Metal-library compilation.
The plugin body calls Mesa/MSC and does not directly create an MTLDevice; that alone
does not prove a closed-source MSC implementation never initializes Metal. Any
future harness that loads MoltenVK's route selector creates a device and must queue.

## Mode selection and MSL comparison

Fast=0, Safe=1, Relaxed=2 are plugin ABI values. Both routes use the shared
`MVKShaderMathPolicy.h`. The IR parser starts with all permissions and intersects
entry-point controls and per-op FPFastMathMode decorations, matching the inspected
SPIRV-Cross parser. `MVKInitialization.cs` selects OnDemand on the shared frontend
path. Consequently, **these legacy corpus modules select Fast by source policy**
when no environment/configuration override exists; Relaxed is not the unconditional
phone or host default. No new device runtime observation is claimed here. The existing CPU-only
`spirv_math_flags_test.cpp` compared all 976 corpus entry points against the pinned
CompilerMSL parser and passed. Its executable links only libc++ and libSystem,
not Metal; this checks parsing/policy agreement, not shader execution. The first
link attempt needed the existing static library namespace override
`MVK_spirv_cross`; no product source was changed to repair that test harness.

`MELONX_METAL_IR_STRICT_MATH=1` forces Safe; any other present string forces Fast.
In particular, the string `2` does not request Relaxed. Build a controlled source
policy or use the compiler ABI to select Relaxed in the future matrix.

| Layer / rule | Fast | Safe | Relaxed | Required behavior / assessment | Risk / oracle |
| --- | --- | --- | --- | --- | --- |
| Entry mode mapping | Fast flags | no fast permissions | NSZ/ARCP/REASSOC/CONTRACT | Shared resolver agrees structurally with MSL; preserve operation controls separately. MATCH (source). | R14 / O-POLICY |
| Mesa `disable_math_refactoring` | false | true | true | Consistent with the selected module policy; not a blanket NIR guarantee. MATCH (source). | R03 / O-EXACT |
| Mesa `relaxed_math_refactoring` | false | false | true | Relaxed marker is deliberate. MATCH (source). | R03 / O-EXACT |
| Source NoContraction | NIR exact | NIR exact | NIR exact | `vtn_handle_fp_fast_math` ORs `nir_fp_exact`; binary emitter then uses zero flags. End-to-end preservation remains RISK. | R03 / O-EXACT |
| Per-op FPFastMathMode | read | read | read | Overrides type defaults; six NIR controls are represented. AllowRecip lacks an independent NIR control in this path; test absent/present flag combinations. RISK. | R03/R06 / O-EXACT |
| Float-controls execution modes | read into NIR info | same | same | Denorm/RTE/RTZ/SZInfNaN are parsed; final hardware behavior is a separate obligation. RISK. | R04/R10 / O-CONTROLS |
| Native DenormPreserve / RTZ | rejected | rejected | rejected | Not advertised; explicit rejection is a supported-feature boundary, not silent wrong rendering. MATCH (source scope). | R04 / O-CONTROLS |
| DXIL denorm attribute | FP32 ftz/preserve if declared | same | same | Only FP32 function attribute is emitted here. Half/other widths and converter consumption need checks. RISK. | R04 / O-CONTROLS |
| Non-exact floating binary flags | UNSAFE_ALGEBRA | 0 | NSZ + ARCP | Modes differ only at this emission boundary; instruction-specific source permissions must not be broadened. RISK. | R03 / O-EXACT |
| Exact binary flags | 0 | 0 | 0 | Guard is retained after 0006. MATCH (source scope), pending converter/GPU checks. | R03 / O-EXACT |
| Intrinsic calls / casts / compares | no binary FMF added | same | same | They do not use the same binop flag path; precision cannot be inferred from the binary flag guard. RISK. | R04–R10 |
| AIR permission adapter | not invoked | not invoked | marker 0x0c -> 0xcc | Only BINOP records with exactly NSZ/ARCP are edited; no CALL rewrite. It has no source-ID/exact provenance map. RISK. | R03 / O-AIR |
| AIR adapter added permissions | none | none | reassoc + contract | Does not add nnan/ninf/afn/legacy unsafe-algebra; structural fact, not numerical acceptance. MATCH (source scope). | R03 / O-AIR |
| MSL math mode (selector available) | Fast / fast functions | Safe / precise functions | Relaxed / precise functions | Correct policy mapping is visible in `getMTLCompileOptions`; library intrinsic differences still need controls. RISK. | R14 / O-POLICY |
| MSL old selector fallback | fastMathEnabled=true | false | false | Old APIs cannot express all three modes; do not call this the same runtime policy as new APIs. RISK. | R14 / O-POLICY |
| Invariant | request flag preserved | same | same | SPIR-V Invariant reaches `preserveInvariance` / MSC PositionInvariance; cross-stage results need oracle. RISK. | R13 / O-IO |
| IR stage acceptance | vertex / fragment / compute | same | same | Other execution models fail explicitly. MATCH (source scope). | R13 / O-IO |
| Automatic MSL fallback | disabled | disabled | disabled | Compilation rejection is a failure category, not successful fallback. MATCH (source scope). | B4 failure manifest |

## Bridge and SPIR-V-to-NIR configuration

| Layer / rule | Fast | Safe | Relaxed | Requirement / assessment | Oracle |
| --- | --- | --- | --- | --- | --- |
| NIR environment | Vulkan | Vulkan | Vulkan | Apply Vulkan interpretation; MATCH (source). | C0 SPIR-V |
| Shader-model ceiling | 6.6 | 6.6 | 6.6 | Valid overloads and converter support remain required; RISK. | R01/R12 |
| DXIL validation request | NO_DXIL_VALIDATION | same | same | No external DXIL-validator success may be inferred from translation success; RISK. | B4 failure manifest |
| Supported compiler bit sizes | 16/32/64 | same | same | Runtime capability exposure is narrower; do not test unsupported float64 as a Vulkan obligation. RISK. | O-CONTROLS |
| keep_io_vars | true | true | true | Independent-stage declarations retain stable Vulkan linkage; RISK. | O-IO |
| lower_to_bindless | false | false | false | Explicit descriptor-table path is active; bindless lowering is not. INACTIVE lowering. | O-IO |
| input_clip_size | 0 | 0 | 0 | No previous-stage count is supplied here; exercise clip/cull signatures. RISK. | O-IO |
| First vertex/base instance | runtime data | same | same | Vulkan base values must be applied exactly once; RISK. | O-SYSVAL |
| Workgroup ID origin | zero, or runtime data with dispatch-base flag | same | same | Preserve nonzero dispatch origin when enabled. RISK. | O-SYSVAL |
| Runtime CBV/push CBV/raw SRV spaces | 31 / 30 / 28 | same | same | Binding namespaces must agree with native tables; no floating arithmetic permission follows. RISK. | O-IO |
| Y flip / half-Z adaptation | request-dependent | same | same | Preserve clip-space convention, clipping and invariance. RISK. | O-IO |
| Debug options | zero-initialized | same | same | No diagnostic numerical override is requested here. MATCH (source). | provenance |

## NIR compiler-option matrix

The following **180-field** inventory includes zero-initialized options, not only
explicit initializers. Sixty-five fields are explicitly or dynamically set.
All three math modes use the same NIR option values. The getter receives supported
bit sizes 16|32|64; its optional unsupported-64-bit branches do not run. SM6.6 turns
on signed/unsigned packed 4x8 dot support. Runtime API capability exposure is a
separate constraint (float64 is not advertised).

`float_mul_add*` replaced the older `fuse_ffma*` switches in this pinned revision.
Likewise, `has_ldexp=false` selects ldexp lowering; there is no `lower_ldexp` member
to invent. Some passes are called directly regardless of an option field, notably
scalar ALU lowering, the DXIL quantize pass, frexp lowering and flrp lowering.

For readable rows, a zero means the C initializer's default. RISK is conservative:
a source option's existence does not prove its IEEE or Vulkan behavior through MSC.
Integer/layout transformations have bit-vector/API obligations, not an IEEE claim.
| ID / rule | Fast | Safe | Relaxed | Requirement / rewrite | Assessment | Risk / oracle | KosmicKrisp reference |
| --- | --- | --- | --- | --- | --- | --- | --- |
| N001 `lower_fdiv` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Division to reciprocal times numerator if requested; current FDIV remains direct. | INACTIVE option | R06 / O-DIV | 0 (zero-initialized) |
| N002 `float_mul_add16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Zero: weak half FMA can split; strong FMA has a separate obligation. | RISK | R01 / O-FMA | nir_float_muladd_support_has_ffma |
| N003 `float_mul_add32` | nir_float_muladd_support_keep_weak_ffma | nir_float_muladd_support_keep_weak_ffma | nir_float_muladd_support_keep_weak_ffma | Keep weak FMA (0x08); no fuse bit; patch 0010 selects FMad. | RISK | R01 / O-FMA | nir_float_muladd_support_has_ffma |
| N004 `float_mul_add64` | nir_float_muladd_support_has_ffma | nir_float_muladd_support_has_ffma | nir_float_muladd_support_has_ffma | Strong double FMA flag; runtime does not advertise float64. | RISK | R01 / O-FMA | 0 (zero-initialized) |
| N005 `lower_flrp16` | true | true | true | Lower linear interpolation; verify endpoints, cancellation and source controls. | RISK | R08 / O-LERP | 0 (zero-initialized) |
| N006 `lower_flrp32` | true | true | true | Lower linear interpolation; verify endpoints, cancellation and source controls. | RISK | R08 / O-LERP | 0 (zero-initialized) |
| N007 `lower_flrp64` | true | true | true | Lower linear interpolation; verify endpoints, cancellation and source controls. | RISK | R08 / O-LERP | 0 (zero-initialized) |
| N008 `lower_fpow` | true | true | true | pow -> exp2(log2(x)*y), subject to the operation domain/error allowance. | RISK | R08 / O-TRANSCENDENTAL | 0 (zero-initialized) |
| N009 `lower_fsat` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Zero keeps SATURATE available; min/max can still fold to it. | INACTIVE option | R05 / O-MINMAX | 0 (zero-initialized) |
| N010 `lower_fsqrt` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Zero keeps sqrt intrinsic instead of reciprocal-rsqrt expansion. | INACTIVE option | R06 / O-SQRT | 0 (zero-initialized) |
| N011 `lower_sincos` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Zero avoids the optional polynomial sin/cos lowering. | INACTIVE option | R08 / O-TRANSCENDENTAL | 0 (zero-initialized) |
| N012 `lower_fmod` | true | true | true | fmod/frem -> a-b*floor/trunc(a/b); not an IEEE remainder oracle. | RISK | R08 / O-REM | true |
| N013 `lower_bitfield_extract8` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N014 `lower_bitfield_extract16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N015 `lower_bitfield_extract` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N016 `lower_bitfield_insert` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N017 `lower_bitfield_reverse` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N018 `lower_bit_count` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N019 `lower_ifind_msb` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | true |
| N020 `lower_ufind_msb` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | true |
| N021 `lower_find_lsb` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | INACTIVE option | R09 / O-INTEGER | true |
| N022 `lower_uadd_carry` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | true |
| N023 `lower_usub_borrow` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | true |
| N024 `lower_mul_high` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N025 `lower_mul_high16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N026 `lower_fneg` | true | true | true | negate -> multiply by -1 in the late pass; check signed zero and denorm controls. | RISK | R04 / O-SPECIAL | 0 (zero-initialized) |
| N027 `lower_ineg` | true | true | true | negate -> integer 0-a, preserving modular-width arithmetic. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N028 `lower_fisnormal` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect the operation-specific NaN, infinity, zero and denorm contract. | INACTIVE option | R04 / O-SPECIAL | 0 (zero-initialized) |
| N029 `lower_scmp` | true | true | true | slt/sge/seq/sne -> float predicate plus bool-to-float; retain unordered semantics. | RISK | R04 / O-SPECIAL | true |
| N030 `lower_bitops` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N031 `lower_isign` | true | true | true | Integer sign -> min/max; retain the result at signed extrema. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N032 `lower_fsign` | true | true | true | Float sign -> ordered comparisons, bool-to-float and subtract; use permitted NaN results. | RISK | R04 / O-SPECIAL | 0 (zero-initialized) |
| N033 `lower_iabs` | true | true | true | Integer abs -> max(a,-a); overflow domain follows the source instruction. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N034 `lower_umax` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N035 `lower_umin` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N036 `lower_fminmax_signed_zero` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Zero requests no extra signed-zero correction around native min/max. | INACTIVE option | R05 / O-MINMAX | 0 (zero-initialized) |
| N037 `lower_fdph` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | true |
| N038 `fdot_replicates` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N039 `lower_ffloor` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | False keeps floor; enabling would use subtract/fract. | INACTIVE option | R07 / O-ROUND | 0 (zero-initialized) |
| N040 `lower_ffract` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | False keeps Frc; enabling would use subtract/floor. | INACTIVE option | R07 / O-ROUND | 0 (zero-initialized) |
| N041 `lower_fceil` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | False keeps ceil; enabling would use negate/floor/negate. | INACTIVE option | R07 / O-ROUND | 0 (zero-initialized) |
| N042 `lower_ftrunc` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | False keeps Round_z; double lowering is separately selected. | INACTIVE option | R07 / O-ROUND | 0 (zero-initialized) |
| N043 `lower_fround_even` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | False avoids the documented DX9-only non-even approximation. | MATCH (source scope) | R07 / O-ROUND | 0 (zero-initialized) |
| N044 `lower_pack_half_2x16` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | true |
| N045 `lower_pack_unorm_2x16` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | 0 (zero-initialized) |
| N046 `lower_pack_snorm_2x16` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | 0 (zero-initialized) |
| N047 `lower_pack_unorm_4x8` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | 0 (zero-initialized) |
| N048 `lower_pack_snorm_4x8` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | 0 (zero-initialized) |
| N049 `lower_pack_64_2x32` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | INACTIVE option | R09 / O-INTEGER | true |
| N050 `lower_pack_64_4x16` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | true |
| N051 `lower_pack_32_2x16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N052 `lower_pack_64_2x32_split` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | true |
| N053 `lower_pack_32_2x16_split` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N054 `lower_unpack_half_2x16` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | true |
| N055 `lower_unpack_unorm_2x16` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | 0 (zero-initialized) |
| N056 `lower_unpack_snorm_2x16` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | 0 (zero-initialized) |
| N057 `lower_unpack_unorm_4x8` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | 0 (zero-initialized) |
| N058 `lower_unpack_snorm_4x8` | true | true | true | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | 0 (zero-initialized) |
| N059 `lower_unpack_64_2x32_split` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | true |
| N060 `lower_unpack_32_2x16_split` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N061 `lower_pack_split` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | INACTIVE option | R09 / O-INTEGER | true |
| N062 `lower_extract_byte` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | true |
| N063 `lower_extract_word` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | true |
| N064 `lower_insert_byte` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | true |
| N065 `lower_insert_word` | true | true | true | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | true |
| N066 `vertex_id_zero_based` | true | true | true | Zero-based DXIL IDs require the runtime base/absolute-ID adaptation. | RISK | R13 / O-SYSVAL | 0 (zero-initialized) |
| N067 `lower_base_vertex` | true (runtime-data policy) | true (runtime-data policy) | true (runtime-data policy) | Runtime-data path enables base-vertex lowering; native absolute-ID patch also participates. | RISK | R13 / O-SYSVAL | 0 (zero-initialized) |
| N068 `instance_id_includes_base_index` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N069 `lower_helper_invocation` | true | true | true | Lower HelperInvocation with pixel/helper-lane semantics. | RISK | R12 / O-DERIVATIVE | 0 (zero-initialized) |
| N070 `optimize_sample_mask_in` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N071 `optimize_load_front_face_fsign` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N072 `optimize_quad_vote_to_reduce` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N073 `lower_cs_local_index_to_id` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N074 `lower_cs_local_id_to_index` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N075 `has_cs_global_id` | true | true | true | Use native global dispatch ID with the bridge dispatch-base contract. | RISK | R13 / O-SYSVAL | true |
| N076 `lower_device_index_to_zero` | true | true | true | Single-device operation: replace DeviceIndex with zero. | MATCH (source scope) | R13 / O-SYSVAL | true |
| N077 `lower_wpos_pntc` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N078 `lower_hadd` | true | true | true | Integer average -> bit operations/add/shift without overflowing the intermediate. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N079 `lower_hadd64` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N080 `lower_uadd_sat` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N081 `lower_usub_sat` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N082 `lower_iadd_sat` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N083 `lower_mul_32x16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | INACTIVE option | R09 / O-INTEGER | 0 (zero-initialized) |
| N084 `lower_bfloat16_conversions` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect supported conversion widths, representable ranges and explicit rounding controls. | INACTIVE option | R10 / O-CONVERT | 0 (zero-initialized) |
| N085 `has_f2u_sat` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect supported conversion widths, representable ranges and explicit rounding controls. | RISK | R10 / O-CONVERT | 0 (zero-initialized) |
| N086 `has_f2i_sat` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect supported conversion widths, representable ranges and explicit rounding controls. | RISK | R10 / O-CONVERT | 0 (zero-initialized) |
| N087 `vectorize_tess_levels` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N088 `lower_to_scalar` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N089 `lower_to_scalar_filter` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N090 `vectorize_vec2_16bit` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N091 `unify_interfaces` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N092 `ignore_none_interpolation_in_sysval_gathering` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N093 `lower_interpolate_at` | true | true | true | Lower explicit interpolation calls to barycentric/load forms. | RISK | R12 / O-DERIVATIVE | 0 (zero-initialized) |
| N094 `lower_mul_2x32_64` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | true |
| N095 `has_ldexp` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | False lowers ldexp via bounded exponent construction and multiplies. | RISK | R08 / O-LDEXP | 0 (zero-initialized) |
| N096 `has_rotate8` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N097 `has_rotate16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N098 `has_rotate32` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N099 `has_shfr32` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N100 `has_iadd3` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N101 `has_amul` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N102 `has_imul24` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N103 `has_umul24` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N104 `has_mul24_relaxed` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N105 `has_umul_16x16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N106 `has_imad32` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N107 `has_umad24` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N108 `has_fused_comp_and_csel` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect the operation-specific NaN, infinity, zero and denorm contract. | RISK | R04 / O-SPECIAL | 0 (zero-initialized) |
| N109 `has_icsel_eqz64` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N110 `has_icsel_eqz32` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N111 `has_icsel_eqz16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N112 `has_fneo_fcmpu` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect the operation-specific NaN, infinity, zero and denorm contract. | RISK | R04 / O-SPECIAL | 0 (zero-initialized) |
| N113 `has_ford_funord` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect the operation-specific NaN, infinity, zero and denorm contract. | RISK | R04 / O-SPECIAL | 0 (zero-initialized) |
| N114 `has_fsub` | true | true | true | Preserve native subtraction/reconstruct add-negate as sub. | RISK | R03 / O-EXACT | true |
| N115 `has_isub` | true | true | true | Preserve native integer subtraction. | RISK | R09 / O-INTEGER | true |
| N116 `has_pack_32_4x8` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N117 `has_pixel_coord` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N118 `has_texture_scaling` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N119 `has_sdot_4x8` | true (SM >= 6.4) | true (SM >= 6.4) | true (SM >= 6.4) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N120 `has_udot_4x8` | true (SM >= 6.4) | true (SM >= 6.4) | true (SM >= 6.4) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N121 `has_sudot_4x8` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N122 `has_sdot_4x8_sat` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N123 `has_udot_4x8_sat` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N124 `has_sudot_4x8_sat` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N125 `has_dot_2x16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N126 `has_bfdot2_bfadd` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect supported conversion widths, representable ranges and explicit rounding controls. | RISK | R10 / O-CONVERT | 0 (zero-initialized) |
| N127 `has_fmulz` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect the operation-specific NaN, infinity, zero and denorm contract. | RISK | R04 / O-SPECIAL | 0 (zero-initialized) |
| N128 `has_fmulz_no_denorms` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect the operation-specific NaN, infinity, zero and denorm contract. | RISK | R04 / O-SPECIAL | 0 (zero-initialized) |
| N129 `has_ffmaz_no_denorms` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect the operation-specific NaN, infinity, zero and denorm contract. | RISK | R04 / O-SPECIAL | 0 (zero-initialized) |
| N130 `has_fcanonicalize` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect the operation-specific NaN, infinity, zero and denorm contract. | RISK | R04 / O-SPECIAL | 0 (zero-initialized) |
| N131 `has_find_msb_rev` | true | true | true | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N132 `has_pack_half_2x16_rtz` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Convert, clamp/round where specified, then pack/unpack components in source bit order. | RISK | R10 / O-PACK | 0 (zero-initialized) |
| N133 `has_bit_test` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N134 `has_bfe` | true | true | true | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N135 `has_bfm` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N136 `has_bfi` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N137 `has_bitfield_select` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N138 `has_uclz` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | true |
| N139 `has_msad` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N140 `has_f2e4m3fn_satfn` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect supported conversion widths, representable ranges and explicit rounding controls. | RISK | R10 / O-CONVERT | 0 (zero-initialized) |
| N141 `has_load_global_bounded` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N142 `has_f2i32_rtne` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect supported conversion widths, representable ranges and explicit rounding controls. | RISK | R10 / O-CONVERT | 0 (zero-initialized) |
| N143 `has_atomic_isub` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N144 `has_atomic_load_store` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N145 `intel_vec4` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N146 `avoid_ternary_with_two_constants` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N147 `avoid_ternary_with_fabs` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N148 `support_8bit_alu` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Preserve the defined integer/bit-vector result, width and signedness; exclude poison/undefined inputs. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N149 `support_16bit_alu` | true | true | true | Allow native 16-bit ALU; width/rounding still need end-to-end checks. | RISK | R10 / O-CONVERT | 0 (zero-initialized) |
| N150 `max_unroll_iterations` | 32 | 32 | 32 | Unroll bound 32; preserve control-flow iteration behavior. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N151 `max_unroll_iterations_aggressive` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N152 `max_unroll_iterations_fp64` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N153 `lower_uniforms_to_ubo` | true | true | true | Replace ordinary uniform accesses by equivalent UBO addressing. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N154 `force_indirect_unrolling_sampler` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N155 `no_integers` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N156 `force_indirect_unrolling` | (nir_var_shader_in  /  nir_var_shader_out) | (nir_var_shader_in  /  nir_var_shader_out) | (nir_var_shader_in  /  nir_var_shader_out) | Force input/output indirect access lowering; retained I/O locations must remain stable. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N157 `driver_functions` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N158 `late_lower_int64` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N159 `lower_int64_options` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Zero: 64-bit integer support was passed to the options getter. | INACTIVE option | R09 / O-INTEGER | nir_lower_ufind_msb64  /  nir_lower_subgroup_shuffle64 |
| N160 `lower_doubles_options` | nir_lower_drcp  /  nir_lower_dsqrt  /  nir_lower_drsq  /  nir_lower_dfract  /  nir_lower_dtrunc  /  nir_lower_dfloor  /  nir_lower_dceil  /  nir_lower_dround_even | nir_lower_drcp  /  nir_lower_dsqrt  /  nir_lower_drsq  /  nir_lower_dfract  /  nir_lower_dtrunc  /  nir_lower_dfloor  /  nir_lower_dceil  /  nir_lower_dround_even | nir_lower_drcp  /  nir_lower_dsqrt  /  nir_lower_drsq  /  nir_lower_dfract  /  nir_lower_dtrunc  /  nir_lower_dfloor  /  nir_lower_dceil  /  nir_lower_dround_even | rcp,sqrt,rsq,fract,trunc,floor,ceil,round-even bits selected; not all double lowering. | RISK | R01 / O-FMA | (nir_lower_doubles_options)(~0) |
| N161 `divergence_analysis_options` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N162 `lower_fmulz_with_abs_min` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Respect the operation-specific NaN, infinity, zero and denorm contract. | INACTIVE option | R04 / O-SPECIAL | 0 (zero-initialized) |
| N163 `lower_image_offset_to_range_base` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N164 `lower_atomic_offset_to_range_base` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N165 `preserve_mediump` | true | true | true | Keep mediump conversion markers for the backend rather than eagerly narrowing. | RISK | R10 / O-CONVERT | 0 (zero-initialized) |
| N166 `lower_fquantize2f16` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | False option, but the DXIL-specific quantize pass runs explicitly. | INACTIVE option | R10 / O-CONVERT | true |
| N167 `force_f2f16_rtz` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | False: do not globally force half conversion toward zero. | INACTIVE option | R10 / O-CONVERT | 0 (zero-initialized) |
| N168 `lower_layer_fs_input_to_sysval` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N169 `frag_coord_form` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N170 `compact_arrays` | true | true | true | Compact array representation without changing valid element access. | RISK | R13 / O-IO | true |
| N171 `discard_is_demote` | true | true | true | Retain helper invocations after discard; suppress side effects correctly. | RISK | R12 / O-DERIVATIVE | true |
| N172 `scalarize_ddx` | true | true | true | Split vector derivative operations into scalars; preserve coarse/fine distinction. | RISK | R12 / O-DERIVATIVE | 0 (zero-initialized) |
| N173 `coarse_ddx` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | False: do not request replacement of fine derivatives by coarse. | RISK | R12 / O-DERIVATIVE | 0 (zero-initialized) |
| N174 `per_view_unique_driver_locations` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N175 `compact_view_index` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | RISK | R13 / O-IO | 0 (zero-initialized) |
| N176 `io_options` | nir_io_dont_use_pos_for_non_fs_varyings  /  nir_io_mediump_is_32bit | nir_io_dont_use_pos_for_non_fs_varyings  /  nir_io_mediump_is_32bit | nir_io_dont_use_pos_for_non_fs_varyings  /  nir_io_mediump_is_32bit | Keep Vulkan varying slots; mediump I/O remains 32-bit. | RISK | R13 / O-IO | nir_io_mediump_is_32bit  /  nir_io_non_interpolated_as_uint |
| N177 `skip_lower_packing_ops` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Use shifts/masks/joins for the declared bit lanes; retain signed extraction and valid boundary counts. | RISK | R09 / O-INTEGER | 0 (zero-initialized) |
| N178 `max_varying_expression_cost` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N179 `max_workgroup_invocations` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |
| N180 `max_workgroup_count` | 0 (zero-initialized) | 0 (zero-initialized) | 0 (zero-initialized) | Retain valid indexing, layout, control flow and observable side effects; no arithmetic approximation licensed by this option. | INACTIVE option | R13 / O-IO | 0 (zero-initialized) |

## NIR rewrites and DXIL instruction selection

The main and late algebraic lists contain 2,932 and 469 generated patterns.
A structural enumeration selected 1,433 floating-related patterns (using NIR opcode types); 1,281 lack a
root `~` (blanket exact) guard. This is **not 1,281 bugs**: source predicates,
per-node FP controls and mandatory backend lowering also constrain matches.
Each selected pattern has its search/replacement, condition, control tags and an
oracle-family assignment in the private `algebraic-rule-inventory.json`. Unproved
semantic obligations remain RISK. The audit does not claim a proof of every NIR
rewrite merely because its option was listed.

The NIR source explicitly documents that mandatory backend lowerings can run
without a `~` guard. Safe changes DXIL emission permissions; it does not turn off
these source/NIR rewrites. The following mechanisms are the review priorities for
those individual pattern records.

| Layer / rule | Fast | Safe | Relaxed | Requirement / observed rewrite | Assessment / oracle |
| --- | --- | --- | --- | --- | --- |
| NIR main algebraic pass | repeated | repeated | repeated | Respect source FP controls and each permitted error envelope, including backend lowerings without `~`. | RISK R08 / O-ALG |
| NIR late algebraic pass | repeated | repeated | repeated | Reconstruction/fusion predicates are distinct from the source exact guard. | RISK R03 / O-EXACT |
| Weak f32 FMA | retained -> FMad 46 | same | same | Earlier compatibility regression addressed by 0010; retain the guest/MSL rounding evidence. | PR48 evidence; R01 extension tests |
| Separate f32 mul/add fusion | NIR fuse bit off | off | off | NIR does not request fusion; later Fast/AIR policy may still contract permitted pairs. | RISK R03 / O-EXACT |
| Strong f16/f32 FMA | NIR ffma -> Fma 47 | same | same | `OpFmaKHR` must implement the strong operation. Feature query advertises f16/f32, but 0010 only changes ffma_weak. MSC3.1.1's f32 opcode-47 rejection is recorded in #48. | **RISK R01 / O-STRONG-FMA** |
| Weak half FMA | split by zero option | same | same | Weak GLSL semantics and guest compatibility are separate; half rounding/denorms need sensitive tests. | RISK R01/R10 / O-FMA16 |
| fpow | exp2(log2(x)*y) | same | same | Use the defined positive-domain cases and the Vulkan composite error allowance. | RISK R08 / O-TRANSCENDENTAL |
| flrp | explicit lowering pass for 16/32/64 | same | same | Check endpoints and cancellation, including controls; do not demand an arbitrary algebraic grouping. | RISK R08 / O-LERP |
| fmod/frem | quotient-floor/trunc-subtract | same | same | Test discontinuities with the operation's allowed approximation, not C `remainder`. | RISK R08 / O-REM |
| ldexp | exponent construction + multiplies (`has_ldexp=0`) | same | same | Test supported exponent domain, scaling, overflow classification and declared controls. | RISK R08 / O-LDEXP |
| fneg | late multiply by -1 | same | same | Preserve required sign/special-value behavior; account for denorm policy. | RISK R04 / O-SPECIAL |
| Signed integer modulo | `nir_op_imod -> DXIL_BINOP_UREM` | same | same | Source `OpSMod` needs divisor-sign semantics. Negative inputs require enabled Vulkan maintenance8. Nonzero, non-power-of-two operands are the sensitive path. | **RISK R02 / O-SMOD** |
| Signed/unsigned divide and remainder | SDIV/UDIV/SREM/UREM | same | same | Constant zero-divisor idiv/udiv is replaced by zero; only defined inputs are conformance assertions. | RISK R09 / O-INTEGER |
| Integer shift | count masked to width-1 | same | same | Check in-range counts; masking out-of-range counts is an implementation choice on undefined inputs. | MATCH (source scope), R09 boundary tests |
| firstbit / bitfield | FirstbitHi/SHI/Lo, IBFE/UBFE + lowering | same | same | Verify zero/-1 sentinels, signedness and valid offset/count boundaries. | RISK R09 / O-INTEGER |
| FDiv / reciprocal | binary divide / 1 divided by x | zero FMF | NSZ/ARCP marker | ULP/domain/control contract; reciprocal permission is not unlimited error. | RISK R06 / O-DIV |
| Sqrt / Rsqrt | DXIL intrinsic | same intrinsic | same intrinsic | Required error bounds and supported special-value controls. | RISK R06 / O-SQRT |
| FMin/FMax | DXIL FMIN/FMAX | same | same | NaN choices differ from NMin/NMax; signed-zero obligations depend on effective controls. | RISK R05 / O-MINMAX |
| FClamp / saturate | NIR min/max may fold to SATURATE | same | same | Keep the valid clamp domain and distinguish number-preferring operations. | RISK R05 / O-MINMAX |
| Round / RoundEven / Trunc | Round_NE/NI/PI/Z and lowering | same | same | Half ties, negative ties, large integral values; Round tie choice is not RoundEven's contract. | RISK R07 / O-ROUND |
| Fract | DXIL Frc | same | same | Correct finite result; Inf/NaN expectations require the relevant explicit policy. MSL already documents a separate fract caveat. | RISK R07 / O-ROUND |
| f32 -> f16 | FPTRUNC; explicit pack uses f32tof16 path | same | same | `f2f16` and `f2f16_rtz` share cast selection here; establish reachable legal rounding requests before calling a difference a defect. | RISK R10 / O-CONVERT |
| float <-> integer | FPTOSI/FPTOUI/SITOFP/UITOFP | same | same | Representable inputs and conversion rounding; out-of-range ordinary conversion is not a saturating oracle. | RISK R10 / O-CONVERT |
| pack/unpack | norm clamp/round or bit lanes; half conversion | same | same | Lane order, valid range and declared conversion controls; exact packing differs from numeric conversion. | RISK R09/R10 / O-PACK |
| Derivatives | DXIL coarse/fine X/Y selection, scalarization | same | same | Preserve derivative class and valid quad/control-flow domain; compute derivative support is target-dependent. | RISK R12 / O-DERIVATIVE |
| Undef -> zero / OOB removal | explicit pass | same | same | Valid on undefined reads only; never substitute for a required initialized write or enabled robustness contract. | RISK R13 / O-IO |

No new instruction failure above is classified as GPU-confirmed. R01 and R02 are
specific source-level mismatch candidates, with small RED tests prescribed below.
The 0010 weak-FMA repair remains intact; the audit does not roll it back.

## MSC 3.1.1 and 4.0 evaluation

| Rule | Fast | Safe | Relaxed | Requirement / disposition |
| --- | --- | --- | --- | --- |
| Current MSC version | 3.1.1 | 3.1.1 | 3.1.1 | Unchanged binary dependency. |
| Minimum target on iOS | 17.0, targeted 18.0 retry | same | same | Retry only on iOS18+ and only after the converter reports an iOS18 requirement; cache identity includes policy. RISK R11. |
| Host minimum target | macOS26.0 | same | same | The implementation differs from an overbroad iOS17 comment; record the real branch. RISK R11. |
| GPU family | Metal3 | Metal3 | Metal3 | Match runtime Argument Buffers Tier2/family availability; no inferred phone validation. |
| Current compatibility flags | PositionInvariance when requested | same | same | MSC3.1.1 has no NaN/Inf opt-out flag; numeric behavior must come from evidence. RISK R04/R13. |
| MSC4 compile-time branch | default NaN/Inf optimization retained | DisableNanInfOptimization | DisableNanInfOptimization | Existing conditional code distinguishes modes. Header compatibility does not prove all explicit per-op controls survive. RISK R11. |
| DXIL FMad conversion | prior evidence: air.fma.f32 | same tested family | same tested family | Converter implementation evidence, not a universal DXIL hardware promise. PR48 evidence. |
| DXIL Fma float32 | prior opcode-47 rejection | same gap | same gap | Strong-FMA reachability must be tested independently. RISK R01. |
| AIR representation | binary marker/layout assumptions | same | adapter edits BINOP marker | Re-run structural adapter tests and constrained shaders before any converter upgrade. RISK R03/R11. |

Apple currently lists MSC **4.0 beta**, 2D compute derivatives and default NaN/Inf
optimization. The same official page lists converted-library support from iOS17
on Argument Buffers Tier2 devices. These are separate from the deployment minimum
of the converter dylib itself. [Apple converter documentation](https://developer.apple.com/metal/shader-converter/).

A read-only inspection of the `ios-support-4.0` mirror at
`7dbc33bd3d52f64f774a19fff0326774ed582afe` found an arm64 iOS dylib, 30,147,296 bytes,
SHA-256 `3669c3ac62b3695e0ed4020538bf66a4e3b459337d9e2eca51a4bb8bb7df5887`.
`codesign --verify --strict -R '=anchor apple'` passed. Its LC_BUILD_VERSION says
iOS minimum 13.0 / SDK26.4. The binary was downloaded for inspection only: it was
not loaded, installed, linked into a product, or substituted for 3.1.1.

Thus an Apple-signed iOS 4.0 artifact is available. The iOS17/18 deployment APIs
remain present, but actual converted outputs, AIR layout, ABI/signing integration,
and the current iOS18 retry case are unverified. **Recommendation: retain 3.1.1**
for this audit. A separate migration would need all mode/control oracles, source
corpus compilation, target17/18 checks and separately authorized phone validation.
No upgrade or new NaN/Inf policy is proposed here.

## KosmicKrisp comparison

Reference: official Mesa main `827d538ed9eb53e0a3c23f01ba68f858cdfdbcb3`, read-only
files under `src/kosmickrisp/`. This is newer than the pinned fork's embedded
KosmicKrisp copy; do not mistake the older copy's global Safe configuration for the
current implementation. The matrix above uses the current reference option values.

- `float_mul_add16/32 = has_ffma`; strong NIR FMA maps to MSL fma.
- `lower_doubles_options = ~0`, compared with the DXIL path's selected double mask.
- Half/norm/bit-lane lowering choices overlap in places, but the final targets are
  MSL and DXIL/MSC respectively; copying flags is not a proof of equivalence.
- Current library compilation uses Fast math/Fast functions. `nir_to_msl.c` emits
  per-operation Safe pragmas for no-contract/no-reassociate and selected comparisons,
  Relaxed pragmas for preservation requirements, and precise fp32 intrinsics when
  NaN/Inf preservation is required.
- It explicitly repairs min/max signed-zero behavior with bitwise tie handling.
  This is concrete motivation for R05; it is not permission to add an untested
  equivalent workaround to our compiler.
- Compare/selection preservation workarounds and late resource/derivative lowerings
  must be considered alongside `kk_nir_options`, not just the initializer.

LunarG documents Vulkan1.4 conformance on Apple Silicon/macOS26+ and current
macOS-only SDK delivery. This does not certify the older fork snapshot or our IR
pipeline. [LunarG SDK scope](https://vulkan.lunarg.com/doc/view/latest/mac/getting_started.html).
KosmicKrisp remains a reference, not a replacement or an official-Mesa trial build.

## Ranked RISK backlog and RED criteria

Ranks combine a concrete semantic boundary, valid-input reachability, and this
single corpus's coverage. They are not a count of demonstrated defects.

| ID / priority | Risk and first RED test | Source evidence / affected modes | Next oracle |
| --- | --- | --- | --- |
| R01 / P0 | Strong `OpFmaKHR` f16/f32 is advertised but reaches a distinct opcode-47 path. Verify compile/execution rejection and single-rounding sensitivity; also extend weak FMA to half/vectors. | `MVKDevice.mm` feature query, `vtn_alu.c`, `emit_alu`; all modes. Weak f32 compatibility already has #48 evidence. | O-STRONG-FMA / O-FMA16; retain regroup-sensitive counter, use explicit supported feature and controls. |
| R02 / P1 | Signed modulo maps to unsigned remainder. Start with dynamic `a=-5,b=3`, plus all sign combinations; avoid zero and overflow cases. Do not let constant-folding hide the path. | `SpvOpSMod -> nir_op_imod -> UREM`; all modes; zero occurrences in this corpus. | O-SMOD; normative signed-modulo reference + MSL control. |
| R03 / P1 | NoContraction/individual permissions through required lowering, MSC and AIR marker matching. Include unrelated exact and non-exact ops in one shader. | Binary guard is explicit; AIR marker lacks source provenance; Safe does not disable NIR algebraic passes. | O-EXACT / O-AIR; inspect intermediates, then GPU sensitive inputs. |
| R05 / P1 | NaN and signed-zero min/max/clamp/saturate behavior. Test F and N variants separately under explicit controls and both operand orders. | High corpus frequency; current KosmicKrisp has an explicit signed-zero repair. | O-MINMAX; compare permitted result sets and exact sign where required. |
| R06 / P1 | Divide/reciprocal/sqrt/rsqrt error bounds across exponent ranges and supported special cases. | Direct DXIL ops/intrinsics plus mode-specific permissions. | O-DIV / O-SQRT; ULP envelopes, overflow-safe high-precision reference, sensitivity count. |
| R04 / P1 | Supported SZ/Inf/NaN controls and default denorm behavior across widths, bit-preserving loads/stores and comparisons. | Native rejects DenormPreserve and does not advertise FTZ/RTZ; FP32 DXIL attribute alone is not all-width proof. | O-CONTROLS / O-SPECIAL; distinguish unsupported requests from failures. |
| R10 / P2 | Half conversion, quantize, pack/unpack, integer conversion and the shared f2f16/f2f16_rtz cast path. | Rounding selectors may be lost or unreachable; need legal-source cases. | O-CONVERT / O-PACK; half-ULP boundaries, subnormals and representable integer edges. |
| R08 / P2 | Backend-mandated NIR algebraic lowerings without root exact guards: pow, lerp, mod, ldexp and transcendentals. | Individual private inventory has conditions; no mode-specific NIR bypass. | O-ALG / O-LERP / O-REM / O-LDEXP / O-TRANSCENDENTAL. |
| R07 / P2 | Round/roundEven/trunc/fract at signed half ties and large magnitudes. | Intrinsic selection is visible; special-value policy and downstream code remain unverified. | O-ROUND; permitted tie choices and source-specific zero sign. |
| R09 / P2 | Integer carry/borrow, high multiply, bitfield/sentinel/shift edges. | Exact bit-vector lowerings are not established by float tests. | O-INTEGER; defined-domain vectors and separately labelled undefined diagnostics. |
| R12 / P2 | Fine/coarse derivatives, helper lanes and compute derivative dimensionality. | Scalarization, DXIL selection and MSC target support; 4.0 adds 2D compute derivatives. | O-DERIVATIVE; valid quad/control-flow fixture, CTS subgroup/derivative cases. |
| R13 / P2 | Fork I/O, barycentrics, clip/cull, interlock resource classification, invariance and required memory side effects. | keep_io_vars active; link/bindless inactive; ordering comments are not proofs. | O-IO / O-SYSVAL / O-INTERLOCK; include aliases, branches and actual vertex interpolation. |
| R14 / P2 | Policy names, forced overrides and old MSL API fallback could invalidate the comparison matrix. | Legacy workload selects Fast under OnDemand; strict-math string is only a boolean override. | O-POLICY; record requested and effective mode on both routes. |
| R11 / P3 | MSC4 migration, AIR marker layout, target17/18 retry and cache identity. | Apple-signed iOS artifact verified statically; no new binary used. | O-TARGET; separate migration proposal only after approval. |

The planned C0 comparison should use correctly enabled capabilities and supported
float controls. Default conformant-policy results and deliberately forced unsafe
policy results must be separate. MSL-pass/IR-fail cases enter this backlog; common
failures remain recorded shared limitations under the user's plan. A GPU RED must
be a defined-contract failure, not a malformed fixture or different legal result.

## Original A0/A checkpoint (superseded status)

A0: all 11 fork diffs inspected; source unchanged; MSC4 artifact/target assessment
completed; current KosmicKrisp reference inspected. A: 180 NIR option fields plus
mode/translation/instruction tables recorded, with 14 ranked risk families and
explicit oracle assignments. The two clearest new source mismatch candidates are
R01 and R02. **No new GPU-confirmed violation and no new behavior fix is claimed.**

B1–B3 completed extraction and statistics on 976 host modules. The user then
approved B4 first, R02/R01 RED next, R03/R05 before C0, followed by regression.
The original no-GPU statement and pause at A0/A no longer describe the current
execution state. The frozen source tables above remain the baseline audit.

## Approved follow-up results

### B4 and its fixture correction

The complete matrix has 976 modules × two routes × three requested modes = 5,856
cases. **IR compiles 976/976 per mode.** SPIRV-Cross generates source for 975/976;
one vertex module consumes a subgroup builtin unsupported by the selected
ordinary-vertex MSL configuration. Its original pipeline/stage-capability context
is unavailable, so this is not automatically a game-runtime defect.

The first matrix used zero topology flags and reported two IR fragment failures
with duplicate `texcoord0` inputs. Both consume `PointCoord`. The native compiler
requires its point-raster variant for those inputs. After the helper selected
`MVK_METAL_IR_RENDERING_POINTS` for active fragment PointCoord, both compiled in
all modes; the full matrix was rerun. This was a fixture correction, **not a
production fix**. An independent unused-PointCoord rendering control passed on
both routes, rejecting the earlier unused-interface hypothesis. Original errors
and corrected runs are retained privately.

MSC's indirect MPS/Metal loads were caught by the provider guard. Every complete
matrix therefore ran sequentially through the canonical queue. No dispatch or
pipeline creation is part of B4. MSL math mode does not affect SPIRV-Cross source
generation, and its identical source hashes across requested modes are expected.
There are 900 IR modules with mode-dependent artifact hashes; a binary difference
is not a numerical failure. No fallback was used.

### R02: confirmed, repaired in patch 0011

The **valid** RED fixture enables maintenance8 explicitly, excludes zero divisors
and `INT_MIN/-1`, and loads all inputs dynamically. IR passes 6/20 modulo rows in
each mode while MSL passes 20/20. Both routes pass the signed-remainder and input
integrity controls. DXIL before MSC has `urem`. With 0011, all six route/mode cases
pass 20/20 and DXIL uses signed remainder plus divisor-sign correction.

Official CTS independently fails both the 32-bit and 64-bit maintenance8 SMod
cases on the baseline, then passes with 0011; six positive/remainder controls
remain passing. The corrected full B4 matrix has **zero changed statuses and zero
changed artifact hashes** with 0011. The sample contains no `OpSMod`; this is a
correctness repair, not a demonstrated fix for a current game screenshot.

An earlier diagnostic omitted maintenance8 and is explicitly invalid as a Vulkan
conformance RED. It is not substituted for the subsequent feature-enabled run.
See [the repair and reproduction record](signed-modulo.md).

### R01: RED confirmed; compile candidate withheld

Explicitly enabled `shaderFmaFloat32` and `shaderFmaFloat16` reach MSC opcode 47
and abort in all IR modes; MSL passes the dynamic rounding-sensitive fixtures.
This confirms the advertised strong-FMA path is broken. The fp16 sentinels have
normal results, so denormal flush behavior cannot explain their difference.

A local, unpublished FMad-overload candidate removes the abort and passes 12
route/mode/width combinations. It is **not accepted or included in PR #50**:
AIR inspection still shows `reassoc nsz arcp afn` on the FMA calls, including Safe
compilation. Source permissions and full strong semantics require further work;
four finite sentinels are not a complete acceptance oracle. No global math mode,
weak-FMA rule, MSC version or source SPIR-V workaround was changed to hide this.
The crash RED fixtures and the read-only AIR inspector are retained as audit tools.

### R03 and R05

R03's mixed exact/non-exact fp32 vector fixture passes all six route/mode cases,
with 16 lanes that distinguish fused from separate rounding. The existing scalar
math-policy and FMA regression matrices also pass. This is tested fp32 coverage,
not closure of every precision width or all NIR algebraic patterns.

R05 tests FMin/FMax/FClamp separately from number-preferring NMin/NMax/NClamp,
both operand orders, saturation, NaN, infinity and signed zero. Fast uses only
inputs allowed by its permissions; Relaxed allows either zero sign where NSZ is
present. Safe produces **the same ten signed-zero output mismatches on MSL and
IR**; other tested values pass. These common limitations remain recorded under
the approved scope rather than being relabelled as an IR-only bug.

### C0: first bounded official CTS slice

Source: `KhronosGroup/VK-GL-CTS` commit
`bee83722547054754049485ae6ae8344496c02a6`, with its pinned dependencies. The
126-case list includes 92 fp32 float-controls2 cases, 11 SPIR-V instruction cases,
12 precision cases, eight arithmetic operators, one builtin-function case and
two depth cases. This is a declared slice, not the full groups or certification.

Each of six MSL/IR × Fast/Safe/Relaxed runs completed all 126 cases:

| Result per run | Count |
| --- | ---: |
| Pass | 116 |
| Fail | 8 |
| NotSupported | 2 |
| MSL-pass / IR-fail at a matching mode | 0 |

All eight failures are the same F/N min/max ±0 cases on both routes, including
both per-operation decorations and execution-mode controls. The two unsupported
cases request unsupported float-control properties; they are not passes.
[The checked-in result matrix](evidence/ir-audit-cts-slice.json) preserves exact
case names and statuses, while QPA logs and machine identities stay private.

A diagnostic-only ABI forwarding library selects the IR request mode. A queued
MTLCompileOptions hook selects the MSL mode. Preflight verifies all six effective
modes and dependency loading; this avoids calling OnDemand “Relaxed” when it
actually selected Fast. The harness does not edit CTS SPIR-V or production
policy, and the control library must never be shipped. These are forced-mode
experiments; they do not certify the default policy or performance.

### Regression and remaining gates

0011 alone passes the existing FMA 18/18 and math-policy 48/48 matrices. Compiler
sources are identical across the frozen baseline and the later #49 dormant-gate
removal; the fix branch is rebased onto #49. GPU evidence retains its actual older
native runtime identity instead of being relabelled as a newly built runtime.

The local free-space floor is still below 20 GiB after this task's disposable
Mesa and CTS builds were removed. The compiler bundles, CTS executable/data,
source pins, source code and compact evidence remain. Planned fixed-frame
replays are pending that floor; no fresh full-game or phone acceptance is claimed.
Broader phone/multi-game corpora, other risk families and the unrun CTS groups
remain outside this completed slice. The Mesa fork/pin, MSC 3.1.1 and production
default math policy are unchanged.
