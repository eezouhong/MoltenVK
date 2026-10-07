// Opt-in binding attribution. Raw samples, not scaled estimates. No per-draw logs.
#pragma once
#include "MVKReplayDescriptorTrace.h"
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace mvkreplay {
// Each group has MSL/IR counters. Parts: preparation, descriptor script, remaining binding.
enum class BindingGroup : unsigned { Resources, DrawPreparation, MetalDraw, Residency, Count };
constexpr unsigned bindingCounterCount = unsigned(BindingGroup::Count) * 2;
struct BindingSample {
    uint64_t calls = 0, samples = 0, wallNs = 0, cpuNs = 0, unavailable = 0;
    uint64_t parts[3] = {};
};
inline bool bindingSamplingEnabled() {
    static const bool value = [] {
        const char* p = getenv("MELONX_REPLAY_BINDING_SAMPLING");
        return p && !strcmp(p, "1");
    }();
    return value;
}
struct BindingCounters {
    std::atomic<uint64_t> values[8]{};
    void add(const BindingSample& s) {
        const uint64_t v[] = {s.calls, s.samples, s.wallNs, s.cpuNs, s.unavailable,
                             s.parts[0], s.parts[1], s.parts[2]};
        for (unsigned i = 0; i < 8; ++i) values[i].fetch_add(v[i], std::memory_order_relaxed);
    }
    BindingSample snapshot() const {
        auto get = [&](unsigned i) { return values[i].load(std::memory_order_relaxed); };
        return {get(0), get(1), get(2), get(3), get(4), {get(5), get(6), get(7)}};
    }
};
inline BindingCounters bindingCounters[bindingCounterCount];
inline BindingCounters bindingCalibrationCounters[bindingCounterCount];
struct BindingThreadState {
    BindingSample data[bindingCounterCount];
    BindingSample calibration[bindingCounterCount];
    DescriptorSampler sampler;
    uint64_t pending = 0;
    BindingThreadState() : sampler(uint32_t(descriptorClock(CLOCK_MONOTONIC)) ^
                                  uint32_t(reinterpret_cast<uintptr_t>(this))) {}
    void flush() {
        if (!pending) return;
        for (unsigned i = 0; i < bindingCounterCount; ++i) {
            bindingCounters[i].add(data[i]); bindingCalibrationCounters[i].add(calibration[i]);
            data[i] = {}; calibration[i] = {};
        }
        pending = 0;
    }
    ~BindingThreadState() { flush(); }
};
inline BindingThreadState& bindingThreadState() {
    static thread_local BindingThreadState state;
    return state;
}
class BindingTrace {
    BindingThreadState* state = nullptr;
    BindingSample* sample = nullptr;
    uint64_t wall = 0, cpu = 0, partStart = 0, parts[3] = {};
    unsigned nextPart = 0;
    bool valid = true;
    void beginSample(BindingSample& destination) {
        sample = &destination; ++sample->samples;
        wall = descriptorClock(CLOCK_MONOTONIC);
        cpu = descriptorClock(CLOCK_THREAD_CPUTIME_ID);
        partStart = descriptorClock(CLOCK_MONOTONIC);
        valid = wall && cpu && partStart;
    }
    // Forced sample with no TLS publication or recursive calibration. Uses the
    // same clock/checkpoint/destructor path as the immediately following scope.
    explicit BindingTrace(BindingSample& empty) { ++empty.calls; beginSample(empty); }
public:
    explicit BindingTrace(bool ir, bool enabled = bindingSamplingEnabled(), BindingGroup group = BindingGroup::Resources) {
        if (!enabled || group >= BindingGroup::Count) return;
        state = &bindingThreadState();
        const unsigned index = unsigned(group) * 2 + (ir ? 1 : 0);
        auto& data = state->data[index];
        ++data.calls; ++state->pending;
        if (!state->sampler.next()) return;
        {
            BindingTrace empty(state->calibration[index]);
            empty.checkpoint(); empty.checkpoint();
        }
        beginSample(data);
    }
    void checkpoint() {
        if (!sample) return;
        const uint64_t now = descriptorClock(CLOCK_MONOTONIC);
        if (!now || now < partStart || nextPart >= 3) valid = false;
        else parts[nextPart++] = now - partStart;
        partStart = now;
    }
    ~BindingTrace() {
        if (sample) {
            checkpoint();
            const uint64_t endCpu = descriptorClock(CLOCK_THREAD_CPUTIME_ID);
            const uint64_t endWall = descriptorClock(CLOCK_MONOTONIC);
            if (valid && nextPart == 3 && endCpu >= cpu && endWall >= wall) {
                sample->wallNs += endWall - wall; sample->cpuNs += endCpu - cpu;
                for (unsigned i = 0; i < 3; ++i) sample->parts[i] += parts[i];
            } else ++sample->unavailable;
        }
        if (state && state->pending >= 256) state->flush();
    }
    BindingTrace(const BindingTrace&) = delete;
    BindingTrace& operator=(const BindingTrace&) = delete;
};
inline uint32_t bindingSnapshot(BindingSample* output, uint32_t capacity) {
    if (!bindingSamplingEnabled() || !output || capacity < bindingCounterCount) return 0;
    // Local synchronous replay gets exact completed counts. Other active
    // encoder threads may lag by at most 255 calls, and flush on thread exit.
    bindingThreadState().flush();
    for (unsigned i = 0; i < bindingCounterCount; ++i) output[i] = bindingCounters[i].snapshot();
    return bindingCounterCount;
}
inline uint32_t bindingCalibrationSnapshot(BindingSample* output, uint32_t capacity) {
    if (!bindingSamplingEnabled() || !output || capacity < bindingCounterCount) return 0;
    bindingThreadState().flush();
    for (unsigned i = 0; i < bindingCounterCount; ++i) output[i] = bindingCalibrationCounters[i].snapshot();
    return bindingCounterCount;
}
// The two bounded tables are emitted only at the one-second diagnostics gate.
inline std::string bindingSamplesJSON(uint64_t now, const BindingSample (&samples)[bindingCounterCount],
                                     const BindingSample* calibration = nullptr) {
    std::ostringstream line;
    line << "MELONX_BINDING_TOTALS {\"v\":" << (calibration ? 2 : 1) << ",\"monotonicNs\":" << now
         << ",\"inverseProbability\":" << DescriptorSampler::inverseProbability
         << ",\"maxPendingCallsPerThread\":255";
    auto table = [&](const char* name, const BindingSample* records) {
        line << ",\"" << name << "\":[";
        for (unsigned i = 0; i < bindingCounterCount; ++i) {
            const auto& s = records[i];
            if (i) line << ',';
            line << '[' << s.calls << ',' << s.samples << ',' << s.wallNs << ',' << s.cpuNs << ','
                 << s.unavailable << ',' << s.parts[0] << ',' << s.parts[1] << ',' << s.parts[2] << ']';
        }
        line << ']';
    };
    table("groups", samples);
    if (calibration) table("calibration", calibration);
    line << '}';
    return line.str();
}
}
