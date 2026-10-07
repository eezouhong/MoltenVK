#include "MVKReplayBindingTrace.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <limits>
using namespace mvkreplay;

static void emptyCalls(unsigned count, bool enabled) {
    for (unsigned i = 0; i < count; ++i) {
        BindingTrace trace(i & 1, enabled);
        trace.checkpoint(); trace.checkpoint();
    }
}
int main() {
    setenv("MELONX_REPLAY_BINDING_SAMPLING", "1", 1);
    BindingSample before[bindingCounterCount], after[bindingCounterCount];
    assert(bindingSnapshot(nullptr, 2) == 0);
    assert(bindingSnapshot(before, 1) == 0);
    assert(bindingSnapshot(before, bindingCounterCount) == bindingCounterCount);
    emptyCalls(512, false);
    bindingSnapshot(after, bindingCounterCount);
    assert(after[0].calls == before[0].calls && after[1].calls == before[1].calls);
    std::thread worker([] { emptyCalls(100001, true); }); worker.join();
    emptyCalls(100001, true);
    bindingSnapshot(after, bindingCounterCount);
    assert(after[0].calls == 100002 && after[1].calls == 100000);
    for (unsigned i = 0; i < 2; ++i) {
        auto& sample = after[i];
        assert(sample.samples > 450 && sample.samples < 1150 && !sample.unavailable);
        assert(sample.parts[0] + sample.parts[1] + sample.parts[2] <= sample.wallNs);
    }
    assert(!after[2].calls && !after[3].calls);
    { BindingTrace trace(true, true, BindingGroup::DrawPreparation); trace.checkpoint(); trace.checkpoint(); }
    bindingSnapshot(after, bindingCounterCount);
    assert(!after[2].calls && after[3].calls == 1);
    for (auto group : {BindingGroup::MetalDraw, BindingGroup::Residency}) {
        for (bool ir : {false, true}) {
            BindingTrace trace(ir, true, group); trace.checkpoint(); trace.checkpoint();
        }
    }
    bindingSnapshot(after, bindingCounterCount);
    for (unsigned i = 4; i < bindingCounterCount; ++i) assert(after[i].calls == 1);
    BindingSample maximum[bindingCounterCount];
    const auto max = std::numeric_limits<uint64_t>::max();
    for (auto& s : maximum) s = {max, max, max, max, max, {max, max, max}};
    const auto text = bindingSamplesJSON(max, maximum);
    assert(text.size() < 2048 && text.back() == '}' && text.find('\n') == std::string::npos);
    assert(text.find("\"inverseProbability\":128") != std::string::npos);
    assert(text.find("\"maxPendingCallsPerThread\":255") != std::string::npos);
    size_t position = 0, matches = 0;
    while ((position = text.find("18446744073709551615", position)) != std::string::npos) { ++matches; position += 20; }
    assert(matches == bindingCounterCount * 8 + 1);
    for (bool enabled : {false, true}) {
        bindingSnapshot(before, bindingCounterCount);
        auto start = descriptorClock(CLOCK_THREAD_CPUTIME_ID);
        emptyCalls(2000000, enabled);
        auto ns = descriptorClock(CLOCK_THREAD_CPUTIME_ID) - start;
        bindingSnapshot(after, bindingCounterCount);
        printf("{\"enabled\":%s,\"nsPerCall\":%.3f,\"samples\":[", enabled ? "true" : "false", double(ns)/2000000);
        for (unsigned i = 0; i < 2; ++i) {
            const auto count = after[i].samples - before[i].samples;
            printf("%s{\"count\":%llu,\"cpuNsMean\":%.3f,\"partsNsMean\":[%.3f,%.3f,%.3f]}",
                i ? "," : "", (unsigned long long)count,
                count ? double(after[i].cpuNs - before[i].cpuNs)/count : 0.,
                count ? double(after[i].parts[0] - before[i].parts[0])/count : 0.,
                count ? double(after[i].parts[1] - before[i].parts[1])/count : 0.,
                count ? double(after[i].parts[2] - before[i].parts[2])/count : 0.);
        }
        puts("]}");
    }
}
