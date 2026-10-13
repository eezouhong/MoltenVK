# Dormant IR rollout

Ordinary builds retain the IR implementation but select MSL. `MELONX_EXPERIMENTAL_METAL_IR=1` alone cannot activate IR. The build gate is evaluated before plugin probing and physical-device enumeration; disabled builds preserve MSL feature reporting and descriptor/compilation paths.

CMake defaults `MVK_ENABLE_EXPERIMENTAL_METAL_IR=OFF`. Xcode and other builders default to zero through `MVKShaderRouteConfig.h`; no Debug/Release configuration enables it implicitly. Development testing requires `-DMVK_ENABLE_EXPERIMENTAL_METAL_IR=ON` in CMake, or the equivalent compiler preprocessor definition for Xcode. The runtime request is still required. In an enabled test build, missing/incompatible plugins continue to fail explicitly without MSL fallback.

The ABI preflight returns zero without loading a plugin in a dormant build. IR telemetry, cache statistics, diagnostic controls and compiler memory relief do not activate IR work. A suppressed explicit native request reports reason 3 (rollout disabled), requested=false, selected=MSL, available=false. The snapshot remains immutable. This does not change ABI9 or the five-field route schema.

Product Swift and managed code also have opt-in build gates. Product packaging excludes compiler payloads unless its Swift test gate is present. All components must be deliberately built for IR testing; shipping an IR-capable dylib alone must not expose or activate the feature.

Validation: `Tests/MetalIRReplay/run_route.py --rollout disabled` covers no request, a stale explicit request, missing/wrong-ABI plugins and prewarm. The enabled matrix retains the prior fail-closed cases. Run both matrices through the shared graphics queue, recording exact binaries. This route test is not full game or device acceptance.
