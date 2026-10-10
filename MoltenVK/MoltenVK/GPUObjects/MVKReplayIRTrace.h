#pragma once
// IR-only extension; the shared replay ABI and counters remain unchanged.
#include "MVKReplayTrace.h"

namespace mvkreplay {
#if MVK_REPLAY_TRACE
// Actual CPU payload copied into the IR arena, including direct, indirect and
// push-constant updates. Raw indirect aliases contribute zero bytes.
inline void runtimeParameterBytes(uint64_t bytes) {
    if (mode()==Mode::Coarse) gpu.irTemporaryBytes.fetch_add(bytes,std::memory_order_relaxed);
}
#else
inline constexpr void runtimeParameterBytes(uint64_t) noexcept {}
#endif
}
