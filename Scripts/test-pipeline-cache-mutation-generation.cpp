// SPDX-License-Identifier: Apache-2.0
// Dependency doubles for unmodified production mutation and merge bodies.
#include <atomic>
#include <cstdint>
#include <iostream>
#include <future>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;
using namespace std::chrono_literals;

enum VkResult { VK_SUCCESS = 0 };
using VkPipelineCache = void*;

struct ShaderConfig {
    int value = 0;
    bool matches(const ShaderConfig& other) const;
};

static bool throwOnSentinelMatch = false;

bool ShaderConfig::matches(const ShaderConfig& other) const {
    if (throwOnSentinelMatch && other.value == -1) throw bad_alloc();
    return value == other.value;
}

using SPIRVToMSLConversionConfiguration = ShaderConfig;

struct ShaderModuleKey {
    int value = 0;
    bool operator==(const ShaderModuleKey& other) const { return value == other.value; }
};

namespace std {
template <> struct hash<ShaderModuleKey> {
    size_t operator()(const ShaderModuleKey& key) const { return key.value; }
};
}

struct Repository;
class MVKPipelineCache;

struct Owner {
    Repository* repository = nullptr;
    Repository* getShaderLibraryRepository() { return repository; }
};

struct ShaderLibrary {
    Owner* _owner = nullptr;
    int _shaderConversionResultInfo = 0;
    int _compressedMSL = 0;
    void release() { delete this; }
};

struct DeferredLibrary {
    ShaderConfig shaderConfig;
    int resultInfo = 0;
    int compressedMSL = 0;
};

struct Repository {
    ShaderLibrary* acquire(ShaderModuleKey, ShaderConfig*) { return nullptr; }
    void release(ShaderModuleKey, const ShaderConfig&, ShaderLibrary*) {}
};

class MVKShaderLibraryCache {
  public:
    Owner* _owner = nullptr;
    ShaderModuleKey _shaderModuleKey;
    Repository* _repository = nullptr;
    vector<pair<ShaderConfig, ShaderLibrary*>> _shaderLibraries;
    vector<DeferredLibrary> _deferredShaderLibraries;

    explicit MVKShaderLibraryCache(Owner* owner, ShaderModuleKey key = {})
        : _owner(owner), _shaderModuleKey(key) {}
    explicit MVKShaderLibraryCache(MVKPipelineCache*, ShaderModuleKey key = {})
        : _shaderModuleKey(key) {}

    bool hasShaderLibrary(const ShaderConfig& config) const {
        for (const auto& entry : _shaderLibraries)
            if (entry.first.matches(config)) return true;
        for (const auto& entry : _deferredShaderLibraries)
            if (entry.shaderConfig.matches(config)) return true;
        return false;
    }

    void addDeferredShaderLibrary(const ShaderConfig* config, int result, int msl) {
        if (!config || hasShaderLibrary(*config)) return;
        _deferredShaderLibraries.push_back({*config, result, msl});
    }

    bool merge(MVKShaderLibraryCache* other,
               bool* pLogicalContentChanged = nullptr);
};

using MVKShaderModuleKey = ShaderModuleKey;
using MVKShaderLibrary = ShaderLibrary;

class MVKPipelineCache {
  public:
    Owner owner;
    Repository repository;
    unordered_map<MVKShaderModuleKey, MVKShaderLibraryCache*> _shaderCache;
    size_t _dataSize = 17;
    atomic<uint64_t> _mutationGeneration{0};
    mutex _shaderCacheLock;
    bool _isMergeInternallySynchronized = false;

    MVKPipelineCache() { owner.repository = &repository; }

    ~MVKPipelineCache() {
        for (auto& entry : _shaderCache) delete entry.second;
    }

    MVKShaderLibraryCache* getShaderLibraryCache(MVKShaderModuleKey key) {
        auto*& cache = _shaderCache[key];
        if (!cache) cache = new MVKShaderLibraryCache(&owner, key);
        return cache;
    }

    void addDeferred(int module, int config) {
        ShaderConfig shaderConfig{config};
        getShaderLibraryCache({module})->addDeferredShaderLibrary(
            &shaderConfig, config * 2, config * 3);
    }

    void markDirty();
    void markContentChanged();
    Owner* getDevice() { return &owner; }
    VkResult mergePipelineCaches(
        uint32_t srcCacheCount,
        const VkPipelineCache* pSrcCaches);
    VkResult mergePipelineCachesImpl(
        uint32_t srcCacheCount,
        const VkPipelineCache* pSrcCaches);
};

// @PRODUCTION_SHADER_MERGE@
// @PRODUCTION_MARK_DIRTY@
// @PRODUCTION_MARK_CONTENT_CHANGED@
// @PRODUCTION_REPOSITORY_MERGE@
// @PRODUCTION_MERGE_IMPL@

static void require(bool condition, const char* message) {
    if (!condition) throw runtime_error(message);
}

int main() {
    try {
        MVKPipelineCache dirtyOnly;
        require(dirtyOnly._mutationGeneration == 0,
                "initial generation was not zero");
        dirtyOnly._dataSize = 99;
        dirtyOnly.markDirty();
        require(dirtyOnly._mutationGeneration == 0,
                "markDirty advanced generation");
        require(dirtyOnly._dataSize == 0,
                "markDirty did not invalidate serialized size");
        dirtyOnly.markContentChanged();
        require(dirtyOnly._mutationGeneration == 1,
                "markContentChanged did not advance generation");

        MVKPipelineCache destination;
        MVKPipelineCache firstSource;
        MVKPipelineCache secondSource;
        firstSource.addDeferred(1, 10);
        secondSource.addDeferred(2, 20);
        VkPipelineCache sources[] = {&firstSource, &secondSource};

        require(destination.mergePipelineCaches(2, sources) == VK_SUCCESS,
                "initial merge failed");
        require(destination._mutationGeneration == 1,
                "one merge operation must advance generation once");
        require(destination._dataSize == 0,
                "logical merge must invalidate serialized size");

        destination._dataSize = 99;
        require(destination.mergePipelineCaches(2, sources) == VK_SUCCESS,
                "duplicate merge failed");
        require(destination._mutationGeneration == 1,
                "duplicate merge advanced generation");
        require(destination._dataSize == 99,
                "duplicate merge invalidated serialized size");

        require(destination.mergePipelineCaches(0, nullptr) == VK_SUCCESS,
                "empty merge failed");
        require(destination._mutationGeneration == 1,
                "empty merge advanced generation");

        MVKPipelineCache partialDestination;
        MVKPipelineCache partialSource;
        partialSource.addDeferred(3, 30);
        partialSource.addDeferred(3, -1);
        VkPipelineCache partialSources[] = {&partialSource};
        partialDestination._dataSize = 99;
        throwOnSentinelMatch = true;
        bool mergeThrew = false;
        try {
            partialDestination.mergePipelineCaches(1, partialSources);
        } catch (const bad_alloc&) {
            mergeThrew = true;
        }
        throwOnSentinelMatch = false;
        require(mergeThrew, "partial merge did not exercise exceptional exit");
        require(partialDestination._mutationGeneration == 1,
                "partial merge mutation was not published before rethrow");
        require(partialDestination._dataSize == 0,
                "partial merge mutation retained serialized size");

        cout << "PipelineCacheMutationGeneration PASS\n";
        return 0;
    } catch (const exception& error) {
        cerr << "PipelineCacheMutationGeneration FAIL: " << error.what() << '\n';
        return 1;
    }
}
