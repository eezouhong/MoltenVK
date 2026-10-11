# IR varying linkage repair (product issue #352)

## Problem and first divergence

IR rendered the saved-scene actors after the static-discard repair, but main-menu labels, version/copyright text and Loading text were missing. A complete private one-present Vulkan recording reproduced missing labels with IR and readable labels with MSL using identical inputs.

The foreground glyph draw had valid geometry: replacing only its fragment shader with a constant color produced matching quads. GPU-before readbacks of all three vertex input bindings were byte-identical. Replacing only its fragment shader with a UV visualization showed IR receiving near-white coordinates while MSL received atlas coordinates. Synthetic format, default-component, dynamic-stride and atlas sampling controls did not reproduce this particular divergence.

The vertex shader exported generic Locations 0 and 2; the fragment shader consumed only Location 2. Mesa's DXIL signature used each stage's independently packed `driver_location` as its TEXCOORD semantic index. The fragment's compact row 0 consequently linked to the vertex's Location 0 color instead of Location 2 UV. The first incorrect value was at the interstage interface, before texture sampling.

## Repair and scope

Patch `0009` derives generic TEXCOORD semantic indices from Vulkan Location. Packed register rows and component masks remain unchanged. Builtin semantics, native point annotations and vertex attribute naming keep their existing branches. Array rows continue using the existing semantic-index increment. No title/hash special cases or MSL fallback were added.

This changes generated IR code, so the plugin and patched Mesa must be rebuilt and deployed together. The existing dependency identity hashes the Mesa and MSC binaries; the new Mesa therefore invalidates old IR disk artifacts without deleting user caches. ABI 9 and cache/default limits are unchanged.

## Validation

All GPU experiments used the canonical Debug Tool queue. The public probe writes 256 independently CPU-expected fragment results per case, with Metal API/GPU validation enabled:

| Case | MSL control | IR cold | IR disk restore |
| --- | --- | --- | --- |
| Sparse Locations 0/2, fragment subset | 256/256 | 256/256 | 256/256 |
| Reordered declarations, unused extra varying | 256/256 | 256/256 | 256/256 |
| Generic Location 31 | 256/256 | 256/256 | 256/256 |
| Two-row array | 256/256 | 256/256 | 256/256 |
| Two vec2 component ranges at one Location | 256/256 | 256/256 | 256/256 |
| Flat constant varying | 256/256 | 256/256 | 256/256 |
| Noperspective coordinate gradient | 256/256 | 256/256 | 256/256 |

The old IR compiler independently reproduced 0/256 for the sparse case. The 22-case matrix includes this expected failing control. Cold cases compiled two stages; restored cases restored two and compiled none. Every selected IR case explicitly disabled fallback.

The complete unchanged menu recording rendered readable labels, version and copyright after the repair. Both replay children exited zero. The original helper mistakenly counted PNG files while gfxrecon's default output was BMP; the accounting error and original result are retained, with a corrected result based on the produced BMP. This is not a replay crash.

A normal host run without capture hooks also displayed readable Loading text and all main-menu labels. It loaded the frozen DLC save at the same 322 m objective with Matthew and A visible, readable map/HUD text, zero background pipeline failures and zero IR pipeline rejections. No movement input was sent. First-present and effective route/profile records were valid; normal close exited zero and the exact owned process identity was dead.

Relevant neighbor suites also passed: runtime 24/24, point semantics 14/14, native vertex IDs 2/2, Layer/ViewportIndex 4/4 and static-discard producer/consumer 24/24. These use the rebuilt compiler and recorded native repair binary; they are correctness tests, not performance comparisons.

## Runtime identity and limitations

- Compiler base: native #36 head `7374fae6c161e5cf3ad5bc7758586c351302680f` plus patch 0009; pinned Mesa `7a1fc756809f3bdc9771b54e6036b156389dfc85` and MSC 3.1.1.
- Compiler dependency identity: `61c4cc79be153105fcec72960a0d9e4ffbf48034320c1b7ddaff40a39eab8cbd`.
- Replay native: exact recorded #35 source plus #36's two-file dispatch-lifetime repair; immutable SHA retained locally.
- Normal host native SHA: `be6cebe54af647f02b8faa4c12a9aa65ee7e96ccb07d5b377ccaac063c944a6a` (recorded experimental #35 source, header identity 089; not represented as a tagged #36 build).
- Game/profile/firmware, shaders, resource mappings, readbacks and recording remain private. Only public synthetic fixtures and conclusions are committed.

This establishes the generic sparse-interface failure and its observed repairs on macOS. It does not establish all-game correctness, smooth perspective interpolation with varying W, iOS GPU acceptance or performance. The iOS package on product #341 still pins native #34 and does not yet contain this compiler rebuild. Product #285 and native #24 remain unmodified and unmerged.

### Actual rebuilt compiler hashes

- `libMeloNXMetalIR.dylib`: `bd1f0732cc085d8b81c53aa7bbf104afa6a18c748695d0de94cf48a95c8dfccf`.
- `libspirv_to_dxil.dylib`: `1817437761fbd5a977f5751805b2a8cbaaa4766cbee6514badf79758328619f4`.
- `libmetalirconverter.dylib`: `af26d2aabe35c74d80d5701d5a42d845ae3a776822cc3c87c0703472a71a7752`.
