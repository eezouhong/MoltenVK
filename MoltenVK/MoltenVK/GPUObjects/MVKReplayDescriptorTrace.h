// Opt-in descriptor attribution. Keep hot-path writes unchanged.
#pragma once
#include "MVKReplayConfig.h"
#if MVK_REPLAY_TRACE
#include <atomic>
#include <cstdint>
#include <time.h>

namespace mvkreplay {
enum class DescriptorRangeKind : unsigned { Write, Shadow };
struct DescriptorRangeSample {
    uint64_t calls = 0, samples = 0, wallNs = 0, unavailable = 0;
};
struct DescriptorSampleData {
    uint64_t batches = 0, sampledBatches = 0, wallNs = 0, cpuNs = 0, unavailable = 0;
    DescriptorRangeSample ranges[2];
};
// Independent Bernoulli sampling avoids synchronizing with a recurring binding
// order. The unscaled sampled sum and exact call count are logged separately.
class DescriptorSampler {
    uint32_t state;
public:
    static constexpr uint32_t inverseProbability = 128;
    explicit DescriptorSampler(uint32_t seed) : state(seed ? seed : 1) {}
    bool next() {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return (state & (inverseProbability - 1)) == 0;
    }
};
inline uint64_t descriptorClock(clockid_t clock) {
    timespec value{};
    if (clock_gettime(clock, &value)) return 0;
    return uint64_t(value.tv_sec) * 1000000000 + value.tv_nsec;
}
struct DescriptorSampleCounters {
    struct Range {
        std::atomic<uint64_t> calls{0}, samples{0}, wallNs{0}, unavailable{0};
    };
    std::atomic<uint64_t> batches{0}, sampledBatches{0}, wallNs{0}, cpuNs{0}, unavailable{0};
    Range ranges[2];
    void add(const DescriptorSampleData& data) {
        batches.fetch_add(data.batches, std::memory_order_relaxed);
        sampledBatches.fetch_add(data.sampledBatches, std::memory_order_relaxed);
        wallNs.fetch_add(data.wallNs, std::memory_order_relaxed);
        cpuNs.fetch_add(data.cpuNs, std::memory_order_relaxed);
        unavailable.fetch_add(data.unavailable, std::memory_order_relaxed);
        for (unsigned i = 0; i != 2; ++i) {
            ranges[i].calls.fetch_add(data.ranges[i].calls, std::memory_order_relaxed);
            ranges[i].samples.fetch_add(data.ranges[i].samples, std::memory_order_relaxed);
            ranges[i].wallNs.fetch_add(data.ranges[i].wallNs, std::memory_order_relaxed);
            ranges[i].unavailable.fetch_add(data.ranges[i].unavailable, std::memory_order_relaxed);
        }
    }
    DescriptorSampleData snapshot() const {
        auto load = [](const auto& value) { return value.load(std::memory_order_relaxed); };
        DescriptorSampleData result;
        result.batches = load(batches); result.sampledBatches = load(sampledBatches);
        result.wallNs = load(wallNs);
        result.cpuNs = load(cpuNs); result.unavailable = load(unavailable);
        for (unsigned i = 0; i != 2; ++i) {
            const auto& range = ranges[i];
            result.ranges[i] = {load(range.calls), load(range.samples),
                               load(range.wallNs), load(range.unavailable)};
        }
        return result;
    }
};
inline DescriptorSampleCounters descriptorSamples;
inline thread_local DescriptorSampleData* activeDescriptorBatch = nullptr;
struct DescriptorThreadState {
    DescriptorSampleData data;
    DescriptorSampler sampler;
    DescriptorThreadState() : sampler(uint32_t(descriptorClock(CLOCK_MONOTONIC)) ^
                                     uint32_t(reinterpret_cast<uintptr_t>(this))) {}
    void flush() {
        if (!data.batches) return;
        descriptorSamples.add(data);
        data = {};
    }
    ~DescriptorThreadState() { flush(); }
};
inline DescriptorThreadState& descriptorThreadState() {
    static thread_local DescriptorThreadState state;
    return state;
}
class DescriptorBatchTrace {
    DescriptorThreadState* state = nullptr;
    uint64_t wall = 0, cpu = 0;
    bool sampled = false;
public:
    explicit DescriptorBatchTrace(bool enabled) {
        if (!enabled || activeDescriptorBatch) return;
        state = &descriptorThreadState();
        activeDescriptorBatch = &state->data;
        ++state->data.batches;
        sampled = state->sampler.next();
        if (sampled) {
            ++state->data.sampledBatches;
            wall = descriptorClock(CLOCK_MONOTONIC);
            cpu = descriptorClock(CLOCK_THREAD_CPUTIME_ID);
        }
    }
    ~DescriptorBatchTrace() {
        if (!state) return;
        if (sampled) {
            uint64_t endWall = descriptorClock(CLOCK_MONOTONIC);
            uint64_t endCpu = descriptorClock(CLOCK_THREAD_CPUTIME_ID);
            if (wall && cpu && endWall >= wall && endCpu >= cpu) {
                state->data.wallNs += endWall - wall;
                state->data.cpuNs += endCpu - cpu;
            } else ++state->data.unavailable;
        }
        activeDescriptorBatch = nullptr;
        // Bound snapshot lag to 255 completed APIs per active thread. Flush
        // residual counts on thread exit. No atomic operation per tiny range.
        if (state->data.batches >= 256) state->flush();
    }
    DescriptorBatchTrace(const DescriptorBatchTrace&) = delete;
    DescriptorBatchTrace& operator=(const DescriptorBatchTrace&) = delete;
};
class DescriptorSampleScope {
    DescriptorRangeSample* range = nullptr;
    uint64_t begin = 0;
public:
    DescriptorSampleScope(DescriptorRangeKind kind, bool enabled) {
        if (!enabled || !activeDescriptorBatch) return;
        auto& value = activeDescriptorBatch->ranges[unsigned(kind)];
        ++value.calls;
        if (!descriptorThreadState().sampler.next()) return;
        range = &value; ++value.samples;
        begin = descriptorClock(CLOCK_MONOTONIC);
    }
    ~DescriptorSampleScope() {
        if (!range) return;
        uint64_t end = descriptorClock(CLOCK_MONOTONIC);
        if (begin && end >= begin) range->wallNs += end - begin;
        else ++range->unavailable;
    }
    DescriptorSampleScope(const DescriptorSampleScope&) = delete;
    DescriptorSampleScope& operator=(const DescriptorSampleScope&) = delete;
};
}

#else
#include "MVKReplayStubs.h"
#endif
