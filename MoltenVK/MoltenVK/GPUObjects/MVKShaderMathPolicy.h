#pragma once
#include "mvk_private_api.h"
#include "spirv.hpp"
#include <cstdint>

namespace mvkshader {
enum class MathMode { Safe, Relaxed, Fast };
constexpr uint32_t relaxedMathFlags = uint32_t(spv::FPFastMathModeNSZMask) |
    spv::FPFastMathModeAllowRecipMask | spv::FPFastMathModeAllowReassocMask |
    spv::FPFastMathModeAllowContractMask;
constexpr uint32_t fastMathFlags = relaxedMathFlags |
    spv::FPFastMathModeNotNaNMask | spv::FPFastMathModeNotInfMask;

// Resolve the device preference once. SPIRV-Cross supplies the effective entry
// flags, including the legacy default when no FPFastMathDefault is declared.
inline MathMode resolveMathMode(MVKConfigFastMath preference, uint32_t flags) {
    switch (preference) {
        case MVK_CONFIG_FAST_MATH_ALWAYS: flags = fastMathFlags; break;
        case MVK_CONFIG_FAST_MATH_NEVER: flags = 0; break;
        default: break;
    }
    if ((flags & relaxedMathFlags) != relaxedMathFlags) return MathMode::Safe;
    return (flags & fastMathFlags) == fastMathFlags ? MathMode::Fast : MathMode::Relaxed;
}
}
