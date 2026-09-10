// SPDX-License-Identifier: Apache-2.0
// Dependency doubles for the UNMODIFIED production wrapper and concurrent body.
// The Python runner inserts those bodies, then compiles this translation unit.
#include "MVKShaderLibraryWork.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
using namespace std;
using namespace std::chrono_literals;
struct SPIRVToMSLConversionConfiguration {
    int option = 0;
    bool matches(const SPIRVToMSLConversionConfiguration &o) const { return option == o.option; }
};
struct MVKShaderModuleKey {
    int codeSize = 1, codeHash = 0;
    bool operator==(const MVKShaderModuleKey &o) const {
        return codeSize == o.codeSize && codeHash == o.codeHash;
    }
};
namespace std {
template <> struct hash<MVKShaderModuleKey> {
    size_t operator()(const MVKShaderModuleKey &k) const { return k.codeHash; }
};
} // namespace std
struct MVKShaderModule {
    MVKShaderModuleKey key;
    MVKShaderModuleKey getKey() const { return key; }
};
struct VkPipelineCreationFeedback {
    unsigned flags = 0;
};
constexpr unsigned VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT = 1;
void mvkEnableFlags(unsigned &a, unsigned b) { a |= b; }
struct MVKShaderLibrary {
    atomic<int> references{1};
    int value;
    explicit MVKShaderLibrary(int v) : value(v) {}
    void retain() { ++references; }
    void release() {
        if (--references == 0)
            delete this;
    }
};
struct MVKShaderLibraryRepository {
    MVKShaderLibraryWork<MVKShaderModuleKey> _creationWork;
    mutex lock;
    map<pair<int, int>, pair<MVKShaderLibrary *, int>> entries;
    atomic<int> builds{0};
    MVKShaderLibrary *acquire(int module, int option) {
        lock_guard<mutex> guard(lock);
        auto it = entries.find({module, option});
        if (it == entries.end())
            return nullptr;
        ++it->second.second;
        it->second.first->retain();
        return it->second.first;
    }
    MVKShaderLibrary *publish(int module, int option) {
        lock_guard<mutex> guard(lock);
        auto &e = entries[{module, option}];
        if (!e.first)
            e.first = new MVKShaderLibrary(option);
        ++e.second;
        e.first->retain();
        return e.first;
    }
    void release(int module, int option, MVKShaderLibrary *lib) {
        lock_guard<mutex> guard(lock);
        auto it = entries.find({module, option});
        assert(it != entries.end());
        if (--it->second.second == 0) {
            lib->release();
            entries.erase(it);
        }
        lib->release();
    }
    ~MVKShaderLibraryRepository() { assert(entries.empty()); }
};
struct MVKPipeline {
    MVKShaderLibraryRepository *repo;
    bool noCompile = false;
    function<void()> work = [] {};
    bool fail = false;
    bool shouldFailOnPipelineCompileRequired() const { return noCompile; }
    bool shouldRecordShaderLibraryContributions() const { return false; }
    void recordShaderLibraryContribution(MVKShaderModuleKey,
                                         const SPIRVToMSLConversionConfiguration &,
                                         MVKShaderLibrary *) {}
};
struct Deferred {
    SPIRVToMSLConversionConfiguration shaderConfig;
    int resultInfo = 0, compressedMSL = 1;
};
class MVKShaderLibraryCache {
  public:
    MVKShaderLibraryRepository *_repository;
    MVKShaderModuleKey _shaderModuleKey;
    vector<pair<SPIRVToMSLConversionConfiguration, MVKShaderLibrary *>> _shaderLibraries;
    vector<Deferred> _deferredShaderLibraries;
    MVKShaderLibraryCache(MVKPipeline *p, MVKShaderModuleKey k)
        : _repository(p->repo), _shaderModuleKey(k) {}
    ~MVKShaderLibraryCache() {
        for (auto &e : _shaderLibraries)
            _repository->release(_shaderModuleKey.codeHash, e.first.option, e.second);
    }
    MVKShaderLibrary *findShaderLibrary(SPIRVToMSLConversionConfiguration *c) {
        for (auto &e : _shaderLibraries)
            if (e.first.matches(*c))
                return e.second;
        return nullptr;
    }
    void removeDeferred(const SPIRVToMSLConversionConfiguration &c) {
        for (auto it = _deferredShaderLibraries.begin(); it != _deferredShaderLibraries.end();) {
            if (it->shaderConfig.matches(c))
                it = _deferredShaderLibraries.erase(it);
            else
                ++it;
        }
    }
    void addDeferredShaderLibrary(const SPIRVToMSLConversionConfiguration *c, int r, int bytes) {
        _deferredShaderLibraries.push_back({*c, r, bytes});
    }
    bool adoptShaderLibraryMembership(const SPIRVToMSLConversionConfiguration &c,
                                      MVKShaderLibrary *) {
        auto copy = c;
        if (findShaderLibrary(&copy))
            return false;
        auto *lib = _repository->acquire(_shaderModuleKey.codeHash, c.option);
        if (!lib)
            return false;
        _shaderLibraries.push_back({c, lib});
        removeDeferred(c);
        return true;
    }
    MVKShaderLibrary *getShaderLibrary(SPIRVToMSLConversionConfiguration *c, MVKShaderModule *,
                                       MVKPipeline *p, bool *changed, bool *hit,
                                       VkPipelineCreationFeedback *, uint64_t, bool allow = true) {
        *changed = *hit = false;
        if (auto *lib = findShaderLibrary(c)) {
            *hit = true;
            return lib;
        }
        if (auto *lib = _repository->acquire(_shaderModuleKey.codeHash, c->option)) {
            _shaderLibraries.push_back({*c, lib});
            removeDeferred(*c);
            *changed = *hit = true;
            return lib;
        }
        if (!allow || p->noCompile)
            return nullptr;
        ++_repository->builds;
        p->work();
        if (p->fail)
            return nullptr;
        bool deferred = false;
        for (auto &d : _deferredShaderLibraries)
            deferred |= d.shaderConfig.matches(*c);
        auto *lib = _repository->publish(_shaderModuleKey.codeHash, c->option);
        _shaderLibraries.push_back({*c, lib});
        removeDeferred(*c);
        *changed = true;
        *hit = deferred;
        return lib;
    }
    MVKShaderLibrary *getShaderLibraryConcurrent(SPIRVToMSLConversionConfiguration *,
                                                 MVKShaderModule *, MVKPipeline *,
                                                 VkPipelineCreationFeedback *, uint64_t, mutex &,
                                                 const function<void()> &);
};
struct Device {
    MVKShaderLibraryRepository *repo;
    MVKShaderLibraryRepository *getShaderLibraryRepository() { return repo; }
};
class MVKPipelineCache {
  public:
    Device device;
    mutex _shaderCacheLock;
    bool _isExternallySynchronized = false;
    unsigned changes = 0;
    map<int, unique_ptr<MVKShaderLibraryCache>> views;
    explicit MVKPipelineCache(MVKShaderLibraryRepository &repo) : device{&repo} {}
    Device *getDevice() { return &device; }
    void markDirty() { ++changes; }
    MVKShaderLibraryCache *getShaderLibraryCache(MVKShaderModuleKey k) {
        auto &v = views[k.codeHash];
        if (!v) {
            MVKPipeline p{device.repo};
            v = make_unique<MVKShaderLibraryCache>(&p, k);
        }
        return v.get();
    }
    MVKShaderLibrary *getShaderLibraryImpl(SPIRVToMSLConversionConfiguration *c, MVKShaderModule *m,
                                           MVKPipeline *p, VkPipelineCreationFeedback *fb,
                                           uint64_t time) {
        bool changed, hit;
        auto *lib =
            getShaderLibraryCache(m->key)->getShaderLibrary(c, m, p, &changed, &hit, fb, time);
        if (changed)
            markDirty();
        return lib;
    }
    MVKShaderLibrary *getShaderLibrary(SPIRVToMSLConversionConfiguration *, MVKShaderModule *,
                                       MVKPipeline *, VkPipelineCreationFeedback *, uint64_t);
    MVKShaderLibrary *request(int module, int option, MVKPipeline p) {
        SPIRVToMSLConversionConfiguration c{option};
        MVKShaderModule m{{1, module}};
        return getShaderLibrary(&c, &m, &p, nullptr, 0);
    }
    void import(int module, int option) {
        lock_guard<mutex> l(_shaderCacheLock);
        SPIRVToMSLConversionConfiguration c{option};
        getShaderLibraryCache({1, module})->addDeferredShaderLibrary(&c, 0, 1);
    }
    bool hasDeferred(int module, int option) {
        lock_guard<mutex> l(_shaderCacheLock);
        for (auto &d : getShaderLibraryCache({1, module})->_deferredShaderLibraries)
            if (d.shaderConfig.option == option)
                return true;
        return false;
    }
};
// @PRODUCTION_CONCURRENT@
// @PRODUCTION_WRAPPER@
struct Signal {
    promise<void> promise_;
    shared_future<void> f = promise_.get_future().share();
    void set() { promise_.set_value(); }
    void wait() {
        if (f.wait_for(3s) != future_status::ready)
            throw runtime_error("timed out waiting for signal");
    }
};
struct Release {
    Signal &signal;
    ~Release() {
        try {
            signal.set();
        } catch (...) {
        }
    }
};
void require(bool v, const char *message) {
    if (!v)
        throw runtime_error(message);
}
void independent(bool sameModule, bool cached) {
    MVKShaderLibraryRepository repo;
    MVKPipelineCache cache(repo);
    MVKPipeline normal{&repo};
    int module = sameModule ? 1 : 2;
    if (cached)
        cache.request(module, 99, normal);
    Signal entered, release;
    auto a = async(launch::async, [&] {
        auto p = normal;
        p.work = [&] {
            entered.set();
            release.wait();
        };
        return cache.request(1, 1, p);
    });
    entered.wait();
    auto b = async(launch::async, [&] { return cache.request(module, 99, normal); });
    bool progressed = b.wait_for(150ms) == future_status::ready;
    release.set();
    a.get();
    b.get();
    require(progressed, "independent request blocked by compilation under the cache lock");
}
void noCompile() {
    MVKShaderLibraryRepository repo;
    MVKPipelineCache cache(repo);
    MVKPipeline normal{&repo};
    Signal entered, release;
    auto a = async(launch::async, [&] {
        auto p = normal;
        p.work = [&] {
            entered.set();
            release.wait();
        };
        return cache.request(1, 1, p);
    });
    entered.wait();
    auto b = async(launch::async, [&] {
        auto p = normal;
        p.noCompile = true;
        return cache.request(1, 1, p);
    });
    bool quick = b.wait_for(150ms) == future_status::ready;
    release.set();
    a.get();
    auto *result = b.get();
    require(quick && result == nullptr, "compile-required must not wait or compile");
}
void sameInput() {
    MVKShaderLibraryRepository repo;
    vector<unique_ptr<MVKPipelineCache>> caches;
    for (int i = 0; i < 8; ++i)
        caches.emplace_back(new MVKPipelineCache(repo));
    Signal go;
    vector<future<MVKShaderLibrary *>> futures;
    for (auto &c : caches)
        futures.push_back(async(launch::async, [&, cache = c.get()] {
            go.wait();
            MVKPipeline p{&repo};
            p.work = [] { this_thread::yield(); };
            return cache->request(1, 17, p);
        }));
    go.set();
    MVKShaderLibrary *first = nullptr;
    for (auto &f : futures) {
        auto *lib = f.get();
        if (!first)
            first = lib;
        require(lib == first, "canonical results differ");
    }
    require(repo.builds == 1, "same input compiled more than once");
}
void distinctInput() {
    MVKShaderLibraryRepository repo;
    MVKPipelineCache a(repo), b(repo);
    MVKPipeline p{&repo};
    auto one = async(launch::async, [&] { return a.request(1, 1, p); });
    auto two = async(launch::async, [&] { return b.request(1, 2, p); });
    require(one.get()->value == 1 && two.get()->value == 2 && repo.builds == 2,
            "module gate conflated distinct configurations");
}
void deferredFailure() {
    MVKShaderLibraryRepository repo;
    MVKPipelineCache c(repo);
    MVKPipeline p{&repo};
    c.import(1, 17);
    p.fail = true;
    require(!c.request(1, 17, p) && c.hasDeferred(1, 17), "failed build removed serialized input");
    p.fail = false;
    require(c.request(1, 17, p) && !c.hasDeferred(1, 17), "deferred retry/adoption failed");
}
void exportDuringBuild() {
    MVKShaderLibraryRepository repo;
    MVKPipelineCache c(repo);
    MVKPipeline p{&repo};
    c.import(1, 17);
    Signal entered, release;
    auto a = async(launch::async, [&] {
        auto slow = p;
        slow.work = [&] {
            entered.set();
            release.wait();
        };
        return c.request(1, 17, slow);
    });
    entered.wait();
    auto exported = async(launch::async, [&] { return c.hasDeferred(1, 17); });
    bool quick = exported.wait_for(150ms) == future_status::ready;
    release.set();
    a.get();
    bool valid = exported.get();
    require(quick && valid, "export blocked or lost deferred input while compile ran");
    require(!c.hasDeferred(1, 17), "published view retained stale deferred input");
}
void failureReleases() {
    MVKShaderLibraryRepository repo;
    MVKPipelineCache c(repo);
    MVKPipeline p{&repo};
    p.work = [] { throw runtime_error("injected"); };
    try {
        c.request(1, 17, p);
        throw logic_error("no exception");
    } catch (const runtime_error &) {
    }
    p.work = [] {};
    require(c.request(1, 17, p) != nullptr, "exception poisoned work gate");
}
void churn() {
    MVKShaderLibraryRepository repo;
    MVKPipeline p{&repo};
    for (int i = 0; i < 256; ++i) {
        MVKPipelineCache c(repo);
        require(c.request(i, 1, p) != nullptr, "gate churn failed");
    }
    require(repo.entries.empty(), "scratch/view ownership leaked");
}
int main() {
    int failures = 0;
    vector<pair<string, function<void()>>> tests = {
        {"DifferentModuleMissProgresses", [] { independent(false, false); }},
        {"DifferentModuleHitProgresses", [] { independent(false, true); }},
        {"SameModuleHitBypassesPendingMiss", [] { independent(true, true); }},
        {"CompileRequiredReturnsWithoutWaiting", noCompile},
        {"SameInputAcrossViewsCompilesOnce", sameInput},
        {"DifferentConfigsRemainDifferent", distinctInput},
        {"DeferredFailureRetainsInputForRetry", deferredFailure},
        {"ExportDuringMissRemainsAvailable", exportDuringBuild},
        {"ExceptionReleasesCreationGate", failureReleases},
        {"GateChurnAndViewOwnership", churn}};
    for (auto &t : tests) {
        try {
            t.second();
            cout << t.first << " PASS\n";
        } catch (const exception &e) {
            ++failures;
            cout << t.first << " FAIL: " << e.what() << '\n';
        }
    }
    cout << "failures=" << failures << " total=" << tests.size() << '\n';
    return failures ? 1 : 0;
}
