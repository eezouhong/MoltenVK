# Vertex Layer / ViewportIndex compatibility

A valid XC3 2.2.1 vertex request failed before producing an entry point or metallib. Private bounded capture isolated the exact request (about 5 KiB); SPIR-V validation passed and its hash matched the native rejection. Replaying it unchanged through the original compiler reproduced status 1 with no error string.

The first divergence is Mesa's SPIR-V-to-NIR builtin handling: vertex Layer/ViewportIndex require ShaderViewportIndexLayerEXT in the capability table. That flag was unset despite existing lowering and advertised Vulkan support. Patch0008 enables the existing path. It does not replace or erase Layer writes, add a fallback, change ABI9, or alter MSL/native cache behavior/defaults.

| Check | Original IR | Corrected IR / control |
|---|---|---|
| Exact unchanged game request | rejected, no artifact | main + 6744-byte metallib |
| Independent two-layer color oracle | rejected | IR and MSL 512/512 pixels |
| Independent two-viewport color oracle | rejected | IR and MSL 256/256 pixels |
| Neighbor system-value/cache/selection cases | previously passed | 24/24 passed |
| Neighbor point/topology/restoration cases | previously passed | 14/14 passed |
| iOS dependencies / compiler framework | baseline retained | rebuilt successfully, static identity retained privately |

Graphics evidence uses canonical Debug Tool tickets2249–2253. The compiler-isolation runs kept native d367b1f6 unchanged. The host compiler was built from d367b1f6 plus the exact production patch now committed as5977f646; tests are0be8b472. Initial capture-adapter/dependency setup failures remain outside Git and do not count as product results.

The corrected XC3 process reached the new-game transition; the prior vertex artifact rejection disappeared (10 background failures became9). It exited normally. The nine remaining failures are rasterizer-discard pipeline rejections already tracked by MeloNX333, which this patch intentionally does not change. The final screenshot was a loading/transition frame, so this observation is not proof of complete scene correctness, pixel equality, gameplay performance or the specified saved-scene regression. Device validation remains outstanding.

Raw game shader/request/screenshot artifacts remain private. This addresses MeloNX343. Public tests are independently authored semantic fixtures.
