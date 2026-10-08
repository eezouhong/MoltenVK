#include "MVKReplayDescriptorTrace.h"
#include <cassert>
#include <thread>
#include <vector>
using namespace mvkreplay;
int main() {
    {
        DescriptorBatchTrace off(false);
        DescriptorSampleScope ignored(DescriptorRangeKind::Write, true);
    }
    assert(descriptorSamples.snapshot().batches == 0);
    constexpr unsigned count = 100000, workers = 4, batchSize = 10;
    std::vector<std::thread> threads;
    for (unsigned n = 0; n < workers; ++n) threads.emplace_back([] {
        for (unsigned b = 0; b < count / batchSize; ++b) {
            DescriptorBatchTrace outer(true);
            for (unsigned i = 0; i < batchSize; ++i) {
                DescriptorBatchTrace nested(true);
                DescriptorSampleScope write(DescriptorRangeKind::Write, true);
                DescriptorSampleScope shadow(DescriptorRangeKind::Shadow, true);
            }
            assert(activeDescriptorBatch != nullptr);
        }
    });
    for (auto& thread : threads) thread.join();
    assert(activeDescriptorBatch == nullptr);
    auto value = descriptorSamples.snapshot();
    assert(value.batches == workers * count / batchSize);
    assert(value.sampledBatches > 0 && value.sampledBatches < value.batches);
    assert(value.wallNs > 0 && value.cpuNs > 0 && value.unavailable == 0);
    for (const auto& range : value.ranges) {
        assert(range.calls == workers * count);
        assert(range.samples > range.calls / 160 && range.samples < range.calls / 96);
        assert(range.wallNs > 0 && range.unavailable == 0);
    }
    DescriptorSampler sampler(12345);
    unsigned slots[16]{};
    for (unsigned i = 0; i < count; ++i) if (sampler.next()) ++slots[i % 16];
    for (auto slot : slots) assert(slot > 20);
}
