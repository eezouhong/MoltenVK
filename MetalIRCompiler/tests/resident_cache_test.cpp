#include "../../MoltenVK/MoltenVK/GPUObjects/MVKMetalIRResidentCache.h"
#include <cassert>
#include <atomic>
#include <thread>

struct Artifact { unsigned value; };
using Cache = mvkir::ResidentCache<Artifact>;

int main() {
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
