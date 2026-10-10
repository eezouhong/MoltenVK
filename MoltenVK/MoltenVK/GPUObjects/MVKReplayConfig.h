#pragma once
// Diagnostics are absent from ordinary builds, even if runtime flags are set.
#ifndef MVK_REPLAY_TRACE
#define MVK_REPLAY_TRACE 0
#endif

#if MVK_REPLAY_TRACE
#include <atomic>
namespace mvkreplay {
inline std::atomic<bool> chainSamplingOverride{false};
inline bool chainSamplingEnabled() { return chainSamplingOverride.load(std::memory_order_relaxed); }
inline bool setChainSampling(bool enabled) { return chainSamplingOverride.exchange(enabled, std::memory_order_relaxed); }
}
#else
namespace mvkreplay {
inline constexpr bool chainSamplingEnabled() noexcept { return false; }
inline constexpr bool setChainSampling(bool) noexcept { return false; }
}
#endif
