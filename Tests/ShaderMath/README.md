# Shared shader math policy

The policy is independent of the IR compiler. `shader_math_policy_test.cpp`
checks the complete permission lattice. `run_options.py` extracts the actual
old and current `MVKDevice::getMTLCompileOptions` bodies and compares the Metal
option objects they produce, without compiling a shader or submitting GPU work.
It covers 64 flag combinations plus unknown bits, all three preferences, both
invariance settings, and the modern and legacy SDK code paths.

Build the CPU test with the GPUObjects/API, SPIR-V and Vulkan header directories
on the include path. Run the Metal options check through the shared host queue:

```sh
python3 "$DEBUG_TOOL" queue exec --label "Shared shader math options" -- \
  python3 Tests/ShaderMath/run_options.py --baseline "$PRE_REFACTOR_REVISION" \
    --spirv-headers "$SPIRV_HEADERS_INCLUDE" \
    --vulkan-headers "$VULKAN_HEADERS_INCLUDE" --output "$RESULTS"
```

The baseline must precede the policy refactor. This proves option selection
equivalence, not pixel equivalence or complete shader/compiler conformance.
