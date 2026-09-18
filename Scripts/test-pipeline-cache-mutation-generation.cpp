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

enum VkResult {
    VK_SUCCESS = 0,
    VK_ERROR_INITIALIZATION_FAILED = -3,
    VK_ERROR_FEATURE_NOT_PRESENT = -8,
};
using VkPipelineCache = void*;

static bool throwOnCompaction = false;

struct ShaderConfig {
    int value = 0;
    bool matches(const ShaderConfig& other) const;
    ShaderConfig compactedForCacheStorage() const {
        if (throwOnCompaction) throw bad_alloc();
        return *this;
    }
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
    ShaderConfig cacheConfig;
    bool hasConfig = false;
    void release() { delete this; }
    bool hasCacheConfig() const { return hasConfig; }
    void setCacheConfig(const ShaderConfig& config) {
        cacheConfig = config.compactedForCacheStorage();
        hasConfig = true;
    }
    const ShaderConfig& getCacheConfig() const { return cacheConfig; }
    ShaderConfig& getCacheConfig() { return cacheConfig; }
};

struct DeferredLibrary {
    ShaderConfig shaderConfig;
    int resultInfo = 0;
    int compressedMSL = 0;
};

struct Repository {
    ShaderLibrary* acquire(
        ShaderModuleKey,
        ShaderConfig*,
        ShaderLibrary* = nullptr,
        bool = false) {
        return nullptr;
    }
    void release(ShaderModuleKey, const ShaderConfig&, ShaderLibrary*) {}
};

class MVKShaderLibraryCache {
  public:
    Owner* _owner = nullptr;
    ShaderModuleKey _shaderModuleKey;
    Repository* _repository = nullptr;
    vector<ShaderLibrary*> _shaderLibraries;
    vector<DeferredLibrary> _deferredShaderLibraries;

    explicit MVKShaderLibraryCache(Owner* owner, ShaderModuleKey key = {})
        : _owner(owner), _shaderModuleKey(key) {}
    explicit MVKShaderLibraryCache(MVKPipelineCache*, ShaderModuleKey key = {})
        : _shaderModuleKey(key) {}

    bool hasShaderLibrary(const ShaderConfig& config) const {
        for (const auto* library : _shaderLibraries)
            if (library->getCacheConfig().matches(config)) return true;
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


namespace behavior {

static bool throwOnCompaction = false;

struct ShaderConfig {
    int key = 0;
    int persistentValue = 0;

    bool matches(const ShaderConfig& other) const { return key == other.key; }
    void alignWith(const ShaderConfig& other) { *this = other; }
    ShaderConfig compactedForCacheStorage() const {
        if (throwOnCompaction) throw bad_alloc();
        return *this;
    }
    bool operator==(const ShaderConfig& other) const {
        return key == other.key && persistentValue == other.persistentValue;
    }
};

using SPIRVToMSLConversionConfiguration = ShaderConfig;

struct SPIRVToMSLConversionResultInfo {
    int value = 0;
    bool operator==(const SPIRVToMSLConversionResultInfo& other) const {
        return value == other.value;
    }
};

enum MVKConfigCompressionAlgorithm {
    MVK_CONFIG_COMPRESSION_ALGORITHM_NONE = 0,
    MVK_CONFIG_COMPRESSION_ALGORITHM_LZFSE = 1,
};

template <class C>
struct MVKCompressor {
    vector<uint8_t> _compressed;
    size_t _uncompressedSize = 0;
    MVKConfigCompressionAlgorithm _algorithm =
        MVK_CONFIG_COMPRESSION_ALGORITHM_NONE;

    bool operator==(const MVKCompressor& other) const {
        return _compressed == other._compressed &&
            _uncompressedSize == other._uncompressedSize &&
            _algorithm == other._algorithm;
    }
};

struct SPIRVToMSLConversionResult {
    SPIRVToMSLConversionResultInfo resultInfo;
    MVKCompressor<string> compressedMSL;
    bool resident = true;
};

struct ShaderModuleKey {
    int value = 0;
    bool operator==(const ShaderModuleKey& other) const {
        return value == other.value;
    }
};

using MVKShaderModuleKey = ShaderModuleKey;

struct ShaderModuleKeyHash {
    size_t operator()(const ShaderModuleKey& key) const {
        return static_cast<size_t>(key.value);
    }
};

struct VkPipelineCreationFeedback {
    uint32_t flags = 0;
    uint64_t duration = 0;
};

static constexpr uint32_t
    VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT = 1;

static void mvkEnableFlags(uint32_t& flags, uint32_t enabled) {
    flags |= enabled;
}

static uint64_t mvkGetElapsedNanoseconds(uint64_t) { return 0; }

struct PerformanceStats {
    struct {
        uint64_t shaderLibraryFromCache = 0;
    } shaderCompilation;
};

static void addPerformanceInterval(uint64_t&, uint64_t) {}

class Repository;
struct Device;
class MVKPipelineCache;
class MVKPipeline;
class MVKShaderModule;

class ShaderLibrary {
  public:
    Device* _owner = nullptr;
    SPIRVToMSLConversionResultInfo _shaderConversionResultInfo;
    MVKCompressor<string> _compressedMSL;
    ShaderConfig _cacheConfig;
    bool _hasCacheConfig = false;

    ShaderLibrary(Device* owner, const SPIRVToMSLConversionResult& result)
        : _owner(owner),
          _shaderConversionResultInfo(result.resultInfo),
          _compressedMSL(result.compressedMSL),
          _resident(result.resident) {
        ++liveCount;
    }

    ShaderLibrary(
        Device* owner,
        const SPIRVToMSLConversionResultInfo& resultInfo,
        const MVKCompressor<string> compressedMSL)
        : _owner(owner),
          _shaderConversionResultInfo(resultInfo),
          _compressedMSL(compressedMSL) {
        ++liveCount;
    }

    ShaderLibrary(const ShaderLibrary& other)
        : _owner(other._owner),
          _shaderConversionResultInfo(other._shaderConversionResultInfo),
          _compressedMSL(other._compressedMSL),
          _cacheConfig(other._cacheConfig),
          _hasCacheConfig(other._hasCacheConfig),
          _resident(other._resident) {
        ++liveCount;
    }

    bool isResident() const { return _resident; }
    bool hasCacheConfig() const { return _hasCacheConfig; }
    void setCacheConfig(const ShaderConfig& config) {
        _cacheConfig = config.compactedForCacheStorage();
        _hasCacheConfig = true;
    }
    const ShaderConfig& getCacheConfig() const { return _cacheConfig; }
    ShaderConfig& getCacheConfig() { return _cacheConfig; }
    void retain() { ++_referenceCount; }
    void release() {
        if (--_referenceCount == 0) {
            --liveCount;
            delete this;
        }
    }
    int referenceCount() const { return _referenceCount; }

    static int liveCount;

  private:
    int _referenceCount = 1;
    bool _resident = true;
};

int ShaderLibrary::liveCount = 0;
using MVKShaderLibrary = ShaderLibrary;

struct Device {
    Repository* repository = nullptr;
    VkResult configurationResult = VK_SUCCESS;

    Repository* getShaderLibraryRepository() { return repository; }
    VkResult getConfigurationResult() const { return configurationResult; }
    void clearConfigurationResult() { configurationResult = VK_SUCCESS; }
};

class Repository {
  public:
    struct Entry {
        ShaderModuleKey key;
        ShaderLibrary* library = nullptr;
        int membershipCount = 0;
    };

    ~Repository() {
        for (auto& entry : entries) {
            while (entry.membershipCount-- > 0) entry.library->release();
            entry.library->release();
        }
    }

    ShaderLibrary* seed(
        ShaderModuleKey key,
        const ShaderConfig& config,
        const SPIRVToMSLConversionResultInfo& resultInfo,
        const MVKCompressor<string>& compressedMSL) {
        auto* library = new ShaderLibrary(nullptr, resultInfo, compressedMSL);
        library->setCacheConfig(config);
        library->retain();
        entries.push_back({key, library, 1});
        return library;
    }

    ShaderLibrary* acquire(
        ShaderModuleKey key,
        ShaderConfig* config,
        ShaderLibrary* candidate = nullptr,
        bool = false) {
        if (!config) {
            if (candidate) candidate->release();
            return nullptr;
        }
        if (candidate && !candidate->hasCacheConfig()) {
            candidate->setCacheConfig(*config);
        }
        for (auto& entry : entries) {
            if (entry.key == key && entry.library->getCacheConfig().matches(*config)) {
                config->alignWith(entry.library->getCacheConfig());
                ++entry.membershipCount;
                entry.library->retain();
                if (candidate) candidate->release();
                return entry.library;
            }
        }
        if (!candidate || !candidate->isResident()) {
            if (candidate) candidate->release();
            return nullptr;
        }
        candidate->retain();
        entries.push_back({key, candidate, 1});
        return candidate;
    }

    void release(
        ShaderModuleKey key,
        const ShaderConfig& config,
        ShaderLibrary* library) {
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            if (it->key == key && it->library == library &&
                library->getCacheConfig().matches(config)) {
                --it->membershipCount;
                library->release();
                if (it->membershipCount == 0) {
                    library->release();
                    entries.erase(it);
                }
                return;
            }
        }
        throw runtime_error("repository release mismatch");
    }

    int membershipCount() const {
        int count = 0;
        for (const auto& entry : entries) count += entry.membershipCount;
        return count;
    }

    vector<Entry> entries;
};

static int residentEmplaceCountdown = -1;

template <class T>
class FaultVector {
  public:
    using iterator = typename vector<T>::iterator;
    using const_iterator = typename vector<T>::const_iterator;

    template <class... Args>
    void emplace_back(Args&&... args) {
        if (residentEmplaceCountdown == 0) {
            residentEmplaceCountdown = -1;
            throw bad_alloc();
        }
        if (residentEmplaceCountdown > 0) --residentEmplaceCountdown;
        values.emplace_back(forward<Args>(args)...);
    }

    void push_back(const T& value) {
        if (residentEmplaceCountdown == 0) {
            residentEmplaceCountdown = -1;
            throw bad_alloc();
        }
        if (residentEmplaceCountdown > 0) --residentEmplaceCountdown;
        values.push_back(value);
    }

    iterator begin() { return values.begin(); }
    iterator end() { return values.end(); }
    const_iterator begin() const { return values.begin(); }
    const_iterator end() const { return values.end(); }
    size_t size() const { return values.size(); }
    size_t capacity() const { return values.capacity(); }
    T& operator[](size_t index) { return values[index]; }
    const T& operator[](size_t index) const { return values[index]; }
    T& back() { return values.back(); }

  private:
    vector<T> values;
};

struct DeferredLibrary {
    ShaderConfig shaderConfig;
    SPIRVToMSLConversionResultInfo resultInfo;
    MVKCompressor<string> compressedMSL;
};

using MVKDeferredShaderLibrary = DeferredLibrary;

static bool mvkAreShaderLibraryPersistenceEqual(
    const ShaderConfig& lhsConfig,
    const SPIRVToMSLConversionResultInfo& lhsResultInfo,
    const MVKCompressor<string>& lhsCompressedMSL,
    const ShaderConfig& rhsConfig,
    const SPIRVToMSLConversionResultInfo& rhsResultInfo,
    const MVKCompressor<string>& rhsCompressedMSL) {
    return lhsConfig == rhsConfig &&
        lhsResultInfo == rhsResultInfo &&
        lhsCompressedMSL == rhsCompressedMSL;
}

class MVKShaderModule {
  public:
    ShaderModuleKey key{1};
    SPIRVToMSLConversionResult nextResult;
    bool conversionSucceeds = true;

    ShaderModuleKey getKey() const { return key; }
    bool convert(
        ShaderConfig*,
        SPIRVToMSLConversionResult& result) {
        if (!conversionSucceeds) return false;
        result = nextResult;
        return true;
    }
};

struct MVKPipelineShaderLibraryContribution {
    ShaderModuleKey shaderModuleKey;
    ShaderLibrary* shaderLibrary = nullptr;
};

class MVKPipeline {
  public:
    explicit MVKPipeline(Device* device) : _device(device) {}

    bool shouldFailOnPipelineCompileRequired() const { return failOnCompile; }
    bool shouldRecordShaderLibraryContributions() const { return recordContributions; }
    void recordShaderLibraryContribution(
        ShaderModuleKey key,
        ShaderLibrary* library) {
        _shaderLibraryContributions.push_back({key, library});
        library->retain();
    }
    bool hasValidMTLPipelineStates() const { return valid; }
    Device* getDevice() const { return _device; }

    static void releaseShaderLibraryContributions(
        vector<MVKPipelineShaderLibraryContribution>& contributions);
    VkResult adoptShaderLibrariesInto(
        MVKPipelineCache* destinationPipelineCache,
        uint32_t* pAdoptedShaderLibraryCount);

    Device* _device = nullptr;
    bool failOnCompile = false;
    bool recordContributions = false;
    bool valid = true;
    vector<MVKPipelineShaderLibraryContribution> _shaderLibraryContributions;
};

class MVKShaderLibraryCache {
  public:
    MVKShaderLibraryCache(Device* owner, ShaderModuleKey key)
        : _owner(owner), _shaderModuleKey(key),
          _repository(owner->getShaderLibraryRepository()) {}

    ~MVKShaderLibraryCache() {
        for (auto* library : _shaderLibraries) {
            if (_repository) {
                _repository->release(
                    _shaderModuleKey,
                    library->getCacheConfig(),
                    library);
            } else {
                library->release();
            }
        }
    }

    MVKShaderLibrary* getShaderLibrary(
        ShaderConfig* pShaderConfig,
        MVKShaderModule* shaderModule,
        MVKPipeline* pipeline,
        bool* pCacheRepresentationChanged,
        bool* pLogicalContentChanged,
        bool* pWasCacheHit,
        VkPipelineCreationFeedback* pShaderFeedback,
        uint64_t startTime = 0);
    MVKShaderLibrary* findShaderLibrary(
        ShaderConfig* pShaderConfig,
        VkPipelineCreationFeedback* pShaderFeedback = nullptr,
        uint64_t startTime = 0);
    MVKShaderLibrary* addShaderLibrary(
        const ShaderConfig* pShaderConfig,
        const SPIRVToMSLConversionResult& conversionResult);
    MVKShaderLibrary* addShaderLibrary(
        const ShaderConfig* pShaderConfig,
        const SPIRVToMSLConversionResultInfo& resultInfo,
        const MVKCompressor<string> compressedMSL);
    void addDeferredShaderLibrary(
        const ShaderConfig* pShaderConfig,
        const SPIRVToMSLConversionResultInfo& resultInfo,
        const MVKCompressor<string> compressedMSL);
    MVKShaderLibrary* materializeDeferredShaderLibrary(
        ShaderConfig* pShaderConfig,
        MVKPipeline* pipeline,
        VkPipelineCreationFeedback* pShaderFeedback,
        uint64_t startTime,
        bool* pLogicalContentChanged);
    bool takeDeferredShaderLibrary(
        const ShaderConfig& shaderConfig,
        DeferredLibrary* pDeferred = nullptr);
    bool takeDeferredShaderLibraryForReplacement(
        const ShaderConfig& lookupConfig,
        const ShaderConfig& replacementConfig,
        ShaderLibrary* replacement,
        bool* pLogicalContentChanged);
    bool hasShaderLibrary(const ShaderConfig& shaderConfig) const;
    void addShaderLibraryMembership(
        const ShaderConfig& shaderConfig,
        ShaderLibrary* shaderLibrary);
    bool adoptShaderLibraryMembership(
        const ShaderConfig& shaderConfig,
        ShaderLibrary* shaderLibrary,
        bool* pLogicalContentChanged = nullptr);
    bool merge(
        MVKShaderLibraryCache* other,
        bool* pLogicalContentChanged = nullptr);

    PerformanceStats& getPerformanceStats() { return performanceStats; }

    Device* _owner = nullptr;
    ShaderModuleKey _shaderModuleKey;
    Repository* _repository = nullptr;
    FaultVector<ShaderLibrary*> _shaderLibraries;
    vector<DeferredLibrary> _deferredShaderLibraries;
    PerformanceStats performanceStats;
};

class MVKPipelineCache {
  public:
    explicit MVKPipelineCache(Device* device) : _device(device) {}

    ~MVKPipelineCache() {
        for (auto& entry : _shaderCache) delete entry.second;
    }

    Device* getDevice() const { return _device; }
    MVKShaderLibraryCache* getShaderLibraryCache(ShaderModuleKey key) {
        auto*& cache = _shaderCache[key];
        if (!cache) cache = new MVKShaderLibraryCache(_device, key);
        return cache;
    }
    MVKShaderLibrary* getShaderLibraryImpl(
        ShaderConfig* pContext,
        MVKShaderModule* shaderModule,
        MVKPipeline* pipeline,
        VkPipelineCreationFeedback* pShaderFeedback,
        uint64_t startTime);
    bool adoptShaderLibraryMembership(
        ShaderModuleKey shaderModuleKey,
        const ShaderConfig& shaderConfig,
        ShaderLibrary* shaderLibrary);
    void markDirty();
    void markContentChanged();

    Device* _device = nullptr;
    unordered_map<ShaderModuleKey, MVKShaderLibraryCache*, ShaderModuleKeyHash>
        _shaderCache;
    size_t _dataSize = 17;
    atomic<uint64_t> _mutationGeneration{0};
    mutex _shaderCacheLock;
};

static MVKCompressor<string> payload(uint8_t byte) {
    MVKCompressor<string> compressed;
    compressed._compressed = {byte, static_cast<uint8_t>(byte + 1)};
    compressed._uncompressedSize = 7;
    compressed._algorithm = MVK_CONFIG_COMPRESSION_ALGORITHM_LZFSE;
    return compressed;
}

// @PRODUCTION_SHADER_CACHE_BEHAVIOR@
// @PRODUCTION_PIPELINE_CACHE_BEHAVIOR@
// @PRODUCTION_BEHAVIOR_MARKS@
// @PRODUCTION_RELEASE_CONTRIBUTIONS@
// @PRODUCTION_ADOPT_CONTRIBUTIONS@

static void check(bool condition, const char* message) {
    if (!condition) throw runtime_error(message);
}

static void testFreshCompileAndWarmHit() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Repository repository;
        Device device{&repository};
        MVKPipelineCache cache(&device);
        MVKPipeline pipeline(&device);
        MVKShaderModule module;
        module.key = {10};
        module.nextResult = {{101}, payload(11), true};
        ShaderConfig config{5, 50};

        auto* first = cache.getShaderLibraryImpl(
            &config, &module, &pipeline, nullptr, 0);
        check(first != nullptr, "fresh compile returned no library");
        check(cache._mutationGeneration == 1,
              "fresh compile did not advance generation");
        check(repository.membershipCount() == 1,
              "fresh compile did not publish one ownership membership");

        cache._dataSize = 99;
        auto* second = cache.getShaderLibraryImpl(
            &config, &module, &pipeline, nullptr, 0);
        check(second == first, "warm hit returned a different library");
        check(cache._mutationGeneration == 1,
              "warm hit advanced generation");
        check(cache._dataSize == 99,
              "warm hit invalidated serialized size");
    }
    check(ShaderLibrary::liveCount == liveBefore,
          "fresh compile leaked a shader library");
}

static void testEquivalentDeferredReplacement() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Repository repository;
        Device device{&repository};
        MVKPipelineCache cache(&device);
        MVKPipeline pipeline(&device);
        MVKShaderModule module;
        module.key = {20};
        ShaderConfig persisted{7, 70};
        auto persistentPayload = payload(21);
        repository.seed(module.key, persisted, {201}, persistentPayload);
        cache.getShaderLibraryCache(module.key)->addDeferredShaderLibrary(
            &persisted, {201}, persistentPayload);
        cache._dataSize = 99;

        ShaderConfig request = persisted;
        auto* library = cache.getShaderLibraryImpl(
            &request, &module, &pipeline, nullptr, 0);
        auto* view = cache.getShaderLibraryCache(module.key);
        check(library != nullptr, "equivalent deferred replacement failed");
        check(view->_shaderLibraries.size() == 1 &&
                  view->_deferredShaderLibraries.empty(),
              "equivalent deferred replacement did not change representation");
        check(cache._mutationGeneration == 0,
              "equivalent deferred replacement advanced generation");
        check(cache._dataSize == 0,
              "equivalent deferred replacement retained serialized size");
    }
    check(ShaderLibrary::liveCount == liveBefore,
          "equivalent deferred replacement leaked a library");
}

static void testDifferentDeferredReplacement() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Repository repository;
        Device device{&repository};
        MVKPipelineCache cache(&device);
        MVKPipeline pipeline(&device);
        MVKShaderModule module;
        module.key = {30};
        ShaderConfig deferredConfig{8, 80};
        ShaderConfig canonicalConfig{8, 81};
        repository.seed(module.key, canonicalConfig, {301}, payload(31));
        cache.getShaderLibraryCache(module.key)->addDeferredShaderLibrary(
            &deferredConfig, {302}, payload(32));

        ShaderConfig request = deferredConfig;
        auto* library = cache.getShaderLibraryImpl(
            &request, &module, &pipeline, nullptr, 0);
        check(library != nullptr, "different deferred replacement failed");
        check(cache._mutationGeneration == 1,
              "different deferred replacement did not advance generation");
    }
    check(ShaderLibrary::liveCount == liveBefore,
          "different deferred replacement leaked a library");
}

static void testRealAdoption() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Repository repository;
        Device device{&repository};
        MVKPipelineCache cache(&device);
        ShaderModuleKey key{40};
        ShaderConfig config{9, 90};
        auto* canonical = repository.seed(key, config, {401}, payload(41));

        check(cache.adoptShaderLibraryMembership(key, config, canonical),
              "real adoption rejected a new membership");
        check(cache._mutationGeneration == 1,
              "real adoption did not advance generation");
        check(!cache.adoptShaderLibraryMembership(key, config, canonical),
              "real adoption accepted a duplicate membership");
        check(cache._mutationGeneration == 1,
              "duplicate real adoption advanced generation");
    }
    check(ShaderLibrary::liveCount == liveBefore,
          "real adoption leaked a library");
}

static void testFreshCompileInsertionFault() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Repository repository;
        Device device{&repository};
        MVKPipelineCache cache(&device);
        MVKPipeline pipeline(&device);
        MVKShaderModule module;
        module.key = {50};
        module.nextResult = {{501}, payload(51), true};
        ShaderConfig config{10, 100};

        residentEmplaceCountdown = 0;
        bool threw = false;
        try {
            cache.getShaderLibraryImpl(
                &config, &module, &pipeline, nullptr, 0);
        } catch (const bad_alloc&) {
            threw = true;
        }
        residentEmplaceCountdown = -1;
        check(threw, "fresh compile insertion fault was not injected");
        check(cache._mutationGeneration == 1,
              "fresh compile insertion fault did not publish generation");
        check(cache._dataSize == 0,
              "fresh compile insertion fault retained serialized size");
        check(repository.membershipCount() == 0,
              "fresh compile insertion fault leaked repository membership");
        check(ShaderLibrary::liveCount == liveBefore,
              "fresh compile insertion fault leaked shader ownership");
    }
    check(ShaderLibrary::liveCount == liveBefore,
          "fresh compile fault cleanup changed after destruction");
}

static void testImportInsertionFaultOwnership() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Repository repository;
        Device device{&repository};
        MVKPipelineCache cache(&device);
        ShaderModuleKey key{60};
        ShaderConfig config{11, 110};
        auto* view = cache.getShaderLibraryCache(key);

        residentEmplaceCountdown = 0;
        bool threw = false;
        try {
            view->addShaderLibrary(&config, {601}, payload(61));
        } catch (const bad_alloc&) {
            threw = true;
        }
        residentEmplaceCountdown = -1;
        check(threw, "import insertion fault was not injected");
        check(repository.membershipCount() == 0,
              "import insertion fault leaked repository membership");
        check(ShaderLibrary::liveCount == liveBefore,
              "import insertion fault leaked shader ownership");
    }
}

static void testRepositoryHitInsertionFaultOwnership() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Repository repository;
        Device device{&repository};
        MVKPipelineCache cache(&device);
        ShaderModuleKey key{65};
        ShaderConfig config{14, 140};
        auto* canonical = repository.seed(key, config, {651}, payload(65));
        int membershipBefore = repository.membershipCount();
        int referencesBefore = canonical->referenceCount();
        auto* view = cache.getShaderLibraryCache(key);

        residentEmplaceCountdown = 0;
        bool threw = false;
        try {
            view->addShaderLibrary(
                &config,
                SPIRVToMSLConversionResult{{652}, payload(66), true});
        } catch (const bad_alloc&) {
            threw = true;
        }
        residentEmplaceCountdown = -1;
        check(threw, "repository-hit insertion fault was not injected");
        check(repository.membershipCount() == membershipBefore,
              "repository-hit insertion fault leaked membership");
        check(canonical->referenceCount() == referencesBefore,
              "repository-hit insertion fault leaked retained ownership");
    }
    check(ShaderLibrary::liveCount == liveBefore,
          "repository-hit insertion fault leaked a library");
}

static void testNonRepositoryInsertionFaultOwnership() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Device device{nullptr};
        MVKPipelineCache cache(&device);
        MVKPipeline pipeline(&device);
        MVKShaderModule module;
        module.key = {66};
        module.nextResult = {{661}, payload(67), true};
        ShaderConfig config{15, 150};

        residentEmplaceCountdown = 0;
        bool threw = false;
        try {
            cache.getShaderLibraryImpl(
                &config, &module, &pipeline, nullptr, 0);
        } catch (const bad_alloc&) {
            threw = true;
        }
        residentEmplaceCountdown = -1;
        check(threw, "non-repository insertion fault was not injected");
        check(cache._mutationGeneration == 1,
              "non-repository insertion fault did not publish generation");
        check(ShaderLibrary::liveCount == liveBefore,
              "non-repository insertion fault leaked candidate ownership");
    }
}

static void testNonRepositoryDeferredConfigFaultOwnership() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Device device{nullptr};
        MVKPipelineCache cache(&device);
        MVKPipeline pipeline(&device);
        MVKShaderModule module;
        module.key = {67};
        ShaderConfig config{16, 160};
        auto* view = cache.getShaderLibraryCache(module.key);
        view->addDeferredShaderLibrary(&config, {671}, payload(68));

        throwOnCompaction = true;
        bool threw = false;
        try {
            ShaderConfig request = config;
            cache.getShaderLibraryImpl(
                &request, &module, &pipeline, nullptr, 0);
        } catch (const bad_alloc&) {
            threw = true;
        }
        throwOnCompaction = false;

        check(threw, "deferred config compaction fault was not injected");
        check(view->_shaderLibraries.size() == 0,
              "deferred config fault published a materialized cache entry");
        check(view->_deferredShaderLibraries.size() == 1,
              "deferred config fault consumed the persisted deferred entry");
        check(ShaderLibrary::liveCount == liveBefore,
              "deferred config fault leaked replacement ownership");
    }
    check(ShaderLibrary::liveCount == liveBefore,
          "deferred config fault cleanup changed after destruction");
}

static void testContributionAdoptionExceptionCleanup() {
    int liveBefore = ShaderLibrary::liveCount;
    {
        Repository repository;
        Device device{&repository};
        MVKPipeline pipeline(&device);
        MVKPipelineCache destination(&device);
        ShaderModuleKey firstKey{70};
        ShaderModuleKey secondKey{71};
        ShaderConfig firstConfig{12, 120};
        ShaderConfig secondConfig{13, 130};
        auto* first = repository.seed(firstKey, firstConfig, {701}, payload(71));
        auto* second = repository.seed(secondKey, secondConfig, {702}, payload(72));
        int firstBaseline = first->referenceCount();
        int secondBaseline = second->referenceCount();
        first->retain();
        second->retain();
        pipeline._shaderLibraryContributions.push_back(
            {firstKey, first});
        pipeline._shaderLibraryContributions.push_back(
            {secondKey, second});

        residentEmplaceCountdown = 1;
        bool threw = false;
        uint32_t adoptedCount = 99;
        try {
            pipeline.adoptShaderLibrariesInto(&destination, &adoptedCount);
        } catch (const bad_alloc&) {
            threw = true;
        }
        residentEmplaceCountdown = -1;
        check(threw, "contribution adoption fault was not injected");
        check(pipeline._shaderLibraryContributions.empty(),
              "contribution adoption did not consume the capture batch");
        check(adoptedCount == 0,
              "contribution adoption exception exposed a partial count");
        check(first->referenceCount() == firstBaseline + 1,
              "first captured contribution was not released");
        check(second->referenceCount() == secondBaseline,
              "second captured contribution was not released");
        check(destination._mutationGeneration == 2,
              "partial contribution adoption did not publish both mutations");
        check(repository.membershipCount() == 3,
              "partial contribution adoption retained failed membership");
    }
    check(ShaderLibrary::liveCount == liveBefore,
          "contribution adoption cleanup leaked a library");
}

static void runBehaviorTests() {
    testFreshCompileAndWarmHit();
    testEquivalentDeferredReplacement();
    testDifferentDeferredReplacement();
    testRealAdoption();
    testFreshCompileInsertionFault();
    testImportInsertionFaultOwnership();
    testRepositoryHitInsertionFaultOwnership();
    testNonRepositoryInsertionFaultOwnership();
    testNonRepositoryDeferredConfigFaultOwnership();
    testContributionAdoptionExceptionCleanup();
}

}  // namespace behavior


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

        behavior::runBehaviorTests();

        cout << "PipelineCacheMutationGeneration PASS\n";
        return 0;
    } catch (const exception& error) {
        cerr << "PipelineCacheMutationGeneration FAIL: " << error.what() << '\n';
        return 1;
    }
}
