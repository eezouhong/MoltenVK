#include "../../MoltenVK/MoltenVK/GPUObjects/MVKMetalIRResidentCache.h"
#include "../../MoltenVK/MoltenVK/GPUObjects/MVKShaderRouteConfig.h"
#include <cassert>
#include <atomic>
#include <thread>

struct Artifact { unsigned value; };
using Cache = mvkir::ResidentCache<Artifact>;

int main() {
    auto limits = mvkShaderIRResidentLimits(512, false);
    Cache standard(limits.count, limits.bytes);
    for (unsigned i = 0; i < 513; ++i)
        standard.get(std::to_string(i), [] {
            return Cache::Result{std::make_shared<Artifact>(Artifact{1}), 32 * 1024, false};
        });
    assert(standard.stats().retained == 512 && standard.stats().evictions == 1);
    assert(standard.stats().retainedBytes == 512 * 32 * 1024);
    assert(standard.stats().maxBytes == 64 * 1024 * 1024);

    Cache byteBounded(limits.count, limits.bytes);
    for (unsigned i = 0; i < 200; ++i)
        byteBounded.get(std::to_string(i), [] {
            return Cache::Result{std::make_shared<Artifact>(Artifact{1}), 512 * 1024, false};
        });
    assert(byteBounded.stats().retained == 128 && byteBounded.stats().evictions == 72);

    auto unlimitedLimits = mvkShaderIRResidentLimits(0, false);
    Cache unlimited(unlimitedLimits.count, unlimitedLimits.bytes);
    for (unsigned i = 0; i < 600; ++i)
        unlimited.get(std::to_string(i), [] {
            return Cache::Result{std::make_shared<Artifact>(Artifact{1}), 512 * 1024, false};
        });
    assert(unlimited.stats().retained == 600 && unlimited.stats().evictions == 0);
    assert(unlimited.stats().retainedBytes > 64 * 1024 * 1024);
    auto disabled = mvkShaderIRResidentLimits(512, true);
    auto disabledUnlimited = mvkShaderIRResidentLimits(0, true);
    assert(!disabled.count && !disabled.bytes && !disabledUnlimited.count && !disabledUnlimited.bytes);

    // Read-only statistics must preserve actual budget and ownership. Eviction
    // drops cache ownership, while an active constructing caller remains valid.
    Cache bounded(2, 8);
    auto item = [&](const char* key) {
        return bounded.get(key, [] { return Cache::Result{std::make_shared<Artifact>(Artifact{1}), 4, false}; });
    };
    auto held = item("one"); item("two"); item("three");
    auto stats = bounded.stats();
    assert(stats.maxObjects == 2 && stats.maxBytes == 8);
    assert(stats.retained == 2 && stats.retainedBytes == 8 && stats.evictions == 1);
    assert(item("one") == held && bounded.stats().hits == 1);
    // Exceed a tiny metadata cap while no LRU holds the artifacts. All owners
    // must continue sharing their original library instead of recompiling it.
    Cache live(0, 0, 2);
    unsigned compilations = 0;
    auto request = [&](const std::string& key) {
        return live.get(key, [&] {
            ++compilations;
            return Cache::Result{std::make_shared<Artifact>(Artifact{compilations}), 1, false};
        });
    };
    auto first = request("first");
    std::vector<std::shared_ptr<Artifact>> owners;
    for (unsigned i = 0; i < 200; ++i) owners.push_back(request(std::to_string(i)));
    unsigned before = compilations;
    auto same = request("first");
    assert(same == first && compilations == before);
    assert(live.stats().metadataInspections <= 64 * 201);

    // After the owners release their objects, stale metadata is reclaimable.
    owners.clear(); first.reset(); same.reset();
    for (unsigned i = 0; i < 20; ++i) request("retired-" + std::to_string(i));
    assert(live.stats().metadataPrunes > 0);
    assert(live.stats().entries < 32);

    // Successful concurrent requests still get one result with retention off.
    Cache concurrent(0, 0, 1);
    std::atomic<unsigned> calls{0}, entered{0};
    std::vector<std::shared_ptr<Artifact>> output(8);
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < output.size(); ++i) threads.emplace_back([&, i] {
        ++entered;
        while (entered.load() != output.size()) std::this_thread::yield();
        output[i] = concurrent.get("shared", [&] {
            ++calls;
            for (unsigned n = 0; n < 10000; ++n) std::this_thread::yield();
            return Cache::Result{std::make_shared<Artifact>(Artifact{7}), 1, false};
        });
    });
    for (auto& thread : threads) thread.join();
    assert(calls == 1);
    for (const auto& artifact : output) assert(artifact == output[0]);

    // Deterministic compiler rejection is shared across callers, whereas an
    // allocation/transient failure remains retryable on the same key.
    Cache failures(0, 0);
    unsigned rejected=0;
    for(unsigned i=0;i<100;++i)assert(!failures.get("unsupported",[&] {
        ++rejected;return Cache::Result{{},0,true};
    }));
    assert(rejected==1);
    unsigned retries=0;
    assert(!failures.get("retry",[&] {++retries;return Cache::Result{{},0,false};}));
    auto recovered=failures.get("retry",[&] {
        ++retries;return Cache::Result{std::make_shared<Artifact>(Artifact{9}),1,false};
    });
    assert(recovered&&recovered->value==9&&retries==2);

    calls=0;entered=0;threads.clear();
    for(unsigned i=0;i<8;++i)threads.emplace_back([&] {
        ++entered;
        while(entered.load()!=8)std::this_thread::yield();
        assert(!failures.get("concurrent-rejection",[&] {
            ++calls;return Cache::Result{{},0,true};
        }));
    });
    for(auto& thread:threads)thread.join();
    assert(calls==1);
}
