#include "MVKShaderMathPolicy.h"
#include <cassert>
using namespace mvkshader;
int main() {
    const uint32_t bits[] = {spv::FPFastMathModeNSZMask, spv::FPFastMathModeAllowRecipMask,
        spv::FPFastMathModeAllowReassocMask, spv::FPFastMathModeAllowContractMask,
        spv::FPFastMathModeNotNaNMask, spv::FPFastMathModeNotInfMask};
    // Full permission lattice: preserve the original MSL safe/relaxed/fast
    // classification, including the two explicit device overrides.
    for (unsigned combination = 0; combination < 64; ++combination) {
        uint32_t flags = 0;
        for (unsigned i = 0; i < 6; ++i) if (combination & (1u << i)) flags |= bits[i];
        auto expected = (combination & 15) != 15 ? MathMode::Safe :
            combination == 63 ? MathMode::Fast : MathMode::Relaxed;
        assert(resolveMathMode(MVK_CONFIG_FAST_MATH_ON_DEMAND, flags) == expected);
        assert(resolveMathMode(MVK_CONFIG_FAST_MATH_ALWAYS, flags) == MathMode::Fast);
        assert(resolveMathMode(MVK_CONFIG_FAST_MATH_NEVER, flags) == MathMode::Safe);
    }
    assert(resolveMathMode(MVK_CONFIG_FAST_MATH_ON_DEMAND, UINT32_MAX) == MathMode::Fast);
    assert(resolveMathMode(MVK_CONFIG_FAST_MATH_ON_DEMAND, 0) == MathMode::Safe);
}
