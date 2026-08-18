#!/usr/bin/env python3
from pathlib import Path


def replace_once(path: Path, old: str, new: str) -> None:
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise SystemExit(f"{path}: expected one occurrence, found {count}: {old[:120]!r}")
    path.write_text(text.replace(old, new, 1))


private_api = Path("MoltenVK/MoltenVK/API/mvk_private_api.h")
replace_once(
    private_api,
    "#define MVK_PRIVATE_API_VERSION   44",
    "#define MVK_PRIVATE_API_VERSION   45",
)

stats_block = r'''

/**
 * Logical memory represented by one VkPipelineCache view.
 *
 * The MSL byte counts are exact for MoltenVK-owned serialized source payloads.
 * estimatedViewHostBytes is a lower-bound estimate for C++ view/index storage and
 * deliberately excludes shared MVKShaderLibrary physical payloads and Metal driver
 * allocations.
 */
typedef struct {
    VkBool32 available;
    uint64_t shaderModuleCacheCount;
    uint64_t logicalShaderLibraryCount;
    uint64_t logicalResidentShaderLibraryCount;
    uint64_t specializationVariantCount;
    uint64_t compressedMSLBytes;
    uint64_t uncompressedMSLBytes;
    uint64_t estimatedViewHostBytes;
    uint64_t skippedShaderLibraryCount;
} MVKPipelineCacheMemoryStatistics;

/**
 * Device-wide physical payload owned by the Metal 4 shared shader-library repository.
 *
 * compressedMSLBytes and uncompressedMSLBytes are exact MoltenVK-owned source
 * payload counts. residentUncompressedMSLBytes is a workload-size proxy for
 * resident MTLLibrary payloads, not the Metal driver's allocation size. Metal does
 * not expose per-MTLLibrary allocated bytes; deviceCurrentAllocatedBytes and the
 * process physical footprint must be used to validate real memory release.
 */
typedef struct {
    VkBool32 available;
    uint64_t canonicalShaderLibraryCount;
    uint64_t logicalMembershipCount;
    uint64_t residentCanonicalShaderLibraryCount;
    uint64_t residentShaderLibraryCount;
    uint64_t specializationVariantCount;
    uint64_t compressedMSLBytes;
    uint64_t uncompressedMSLBytes;
    uint64_t residentUncompressedMSLBytes;
    uint64_t estimatedHostBytes;
    uint64_t deviceCurrentAllocatedBytes;
    uint64_t residentLimit;
    uint64_t residentTrimHighWater;
    uint64_t trimCycleCount;
    uint64_t trimBusyCount;
    uint64_t trimCandidateCount;
    uint64_t trimTotalNanoseconds;
    uint64_t trimMaximumNanoseconds;
    uint64_t residentEvictionCount;
    uint64_t evictedUncompressedMSLBytes;
    uint64_t rehydrateCount;
    uint64_t rehydrateFailureCount;
    uint64_t rehydrateTotalNanoseconds;
    uint64_t rehydrateMaximumNanoseconds;
    uint64_t dedupeHitCount;
    uint64_t raceLoserCount;
    uint64_t residentAdoptionCount;
    uint64_t snapshotSkippedShaderLibraryCount;
} MVKMetal4ShaderLibraryRepositoryStatistics;
'''
replace_once(
    private_api,
    "} MVKPerformanceStatistics;\n",
    "} MVKPerformanceStatistics;\n" + stats_block,
)
replace_once(
    private_api,
    "typedef VkResult (VKAPI_PTR *PFN_vkGetPerformanceStatisticsMVK)(VkDevice device, MVKPerformanceStatistics* pPerf, size_t* pPerfSize);",
    "typedef VkResult (VKAPI_PTR *PFN_vkGetPerformanceStatisticsMVK)(VkDevice device, MVKPerformanceStatistics* pPerf, size_t* pPerfSize);\n"
    "typedef VkResult (VKAPI_PTR *PFN_vkGetPipelineCacheMemoryStatisticsMVK)(VkPipelineCache pipelineCache, MVKPipelineCacheMemoryStatistics* pStats, size_t* pStatsSize);\n"
    "typedef VkResult (VKAPI_PTR *PFN_vkGetMetal4ShaderLibraryRepositoryStatisticsMVK)(VkDevice device, MVKMetal4ShaderLibraryRepositoryStatistics* pStats, size_t* pStatsSize);",
)
prototype_marker = r'''VKAPI_ATTR VkResult VKAPI_CALL vkGetPerformanceStatisticsMVK(
	VkDevice                                    device,
	MVKPerformanceStatistics*            		pPerf,
	size_t*                                     pPerfSize);
'''
prototype_addition = prototype_marker + r'''

/** Returns a nonblocking snapshot of one logical VkPipelineCache view. */
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheMemoryStatisticsMVK(
    VkPipelineCache                            pipelineCache,
    MVKPipelineCacheMemoryStatistics*          pStats,
    size_t*                                    pStatsSize);

/** Returns a nonblocking snapshot of the device-wide shared shader repository. */
VKAPI_ATTR VkResult VKAPI_CALL vkGetMetal4ShaderLibraryRepositoryStatisticsMVK(
    VkDevice                                   device,
    MVKMetal4ShaderLibraryRepositoryStatistics* pStats,
    size_t*                                    pStatsSize);
'''
replace_once(private_api, prototype_marker, prototype_addition)

pipeline_h = Path("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h")
replace_once(
    pipeline_h,
    "\tVkResult writeData(size_t* pDataSize, void* pData);\n",
    "\tVkResult writeData(size_t* pDataSize, void* pData);\n\n"
    "\t/** Returns a nonblocking logical memory snapshot for this cache view. */\n"
    "\tvoid getMemoryStatistics(MVKPipelineCacheMemoryStatistics* pStats);\n",
)

shader_h = Path("MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.h")
internal_snapshot = r'''

/** Internal nonblocking snapshot of one physical shader-library payload. */
struct MVKShaderLibraryMemorySnapshot {
    uint64_t shaderLibraryCount = 0;
    uint64_t residentShaderLibraryCount = 0;
    uint64_t specializationVariantCount = 0;
    uint64_t compressedMSLBytes = 0;
    uint64_t uncompressedMSLBytes = 0;
    uint64_t residentUncompressedMSLBytes = 0;
    uint64_t estimatedHostBytes = 0;
};
'''
replace_once(
    shader_h,
    "class MVKShaderLibrary : public MVKBaseDeviceObject {",
    internal_snapshot + "\nclass MVKShaderLibrary : public MVKBaseDeviceObject {",
)
replace_once(
    shader_h,
    "\t/** Returns whether the expensive Metal library payload is currently resident. */\n\tbool isResident() const { return _resident.load(std::memory_order_acquire); }",
    "\t/** Returns whether the expensive Metal library payload is currently resident. */\n"
    "\tbool isResident() const { return _resident.load(std::memory_order_acquire); }\n\n"
    "\t/** Captures known physical payload bytes without waiting on an active library. */\n"
    "\tbool tryGetMemorySnapshot(MVKShaderLibraryMemorySnapshot& snapshot);",
)
replace_once(
    shader_h,
    "\tMVKShaderLibraryCache(MVKVulkanAPIDeviceObject* owner,\n",
    "\t/** Adds this logical view's known bytes to a pipeline-cache snapshot. */\n"
    "\tvoid accumulateMemoryStatistics(MVKPipelineCacheMemoryStatistics* pStats) const;\n\n"
    "\tMVKShaderLibraryCache(MVKVulkanAPIDeviceObject* owner,\n",
)
replace_once(
    shader_h,
    "\tvoid libraryBecameResident(MVKShaderLibrary* library, bool rehydrated);",
    "\tvoid libraryBecameResident(MVKShaderLibrary* library, bool rehydrated, uint64_t rehydrateNanoseconds = 0);\n\n"
    "\t/** Records an attempted cold-entry rehydrate that failed. */\n"
    "\tvoid recordRehydrateFailure(uint64_t rehydrateNanoseconds);",
)
replace_once(
    shader_h,
    "\tsize_t getResidentLimit() const { return _residentLimit; }",
    "\t/** Returns a nonblocking physical repository memory and reclaim snapshot. */\n"
    "\tvoid getMemoryStatistics(MVKMetal4ShaderLibraryRepositoryStatistics* pStats);\n\n"
    "\tsize_t getResidentLimit() const { return _residentLimit; }",
)
replace_once(
    shader_h,
    "\tstd::atomic<uint64_t> _residentEvictionCount { 0 };\n\tstd::atomic<uint64_t> _residentAdoptionCount { 0 };\n\tstd::atomic<uint64_t> _rehydrateCount { 0 };",
    "\tstd::atomic<uint64_t> _residentEvictionCount { 0 };\n"
    "\tstd::atomic<uint64_t> _evictedUncompressedMSLBytes { 0 };\n"
    "\tstd::atomic<uint64_t> _residentAdoptionCount { 0 };\n"
    "\tstd::atomic<uint64_t> _rehydrateCount { 0 };\n"
    "\tstd::atomic<uint64_t> _rehydrateFailureCount { 0 };\n"
    "\tstd::atomic<uint64_t> _rehydrateTotalNanoseconds { 0 };\n"
    "\tstd::atomic<uint64_t> _rehydrateMaximumNanoseconds { 0 };\n"
    "\tstd::atomic<uint64_t> _trimCycleCount { 0 };\n"
    "\tstd::atomic<uint64_t> _trimBusyCount { 0 };\n"
    "\tstd::atomic<uint64_t> _trimCandidateCount { 0 };\n"
    "\tstd::atomic<uint64_t> _trimTotalNanoseconds { 0 };\n"
    "\tstd::atomic<uint64_t> _trimMaximumNanoseconds { 0 };",
)

pipeline_mm = Path("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm")
text = pipeline_mm.read_text()
insert_at = text.index("MVKShaderLibrary* MVKPipelineCache::getShaderLibrary(")
pipeline_stats_impl = r'''
void MVKPipelineCache::getMemoryStatistics(MVKPipelineCacheMemoryStatistics* pStats) {
    if (!pStats) { return; }
    *pStats = {};

    unique_lock<mutex> lock(_shaderCacheLock, try_to_lock);
    if (!lock.owns_lock()) { return; }

    pStats->available = VK_TRUE;
    pStats->shaderModuleCacheCount = _shaderCache.size();
    pStats->estimatedViewHostBytes =
        sizeof(*this) +
        (_shaderCache.bucket_count() * sizeof(void*)) +
        (_shaderCache.size() * sizeof(decltype(_shaderCache)::value_type));

    for (const auto& cacheEntry : _shaderCache) {
        if (cacheEntry.second) {
            cacheEntry.second->accumulateMemoryStatistics(pStats);
        }
    }
}

'''
text = text[:insert_at] + pipeline_stats_impl + text[insert_at:]
pipeline_mm.write_text(text)

shader_mm = Path("MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm")
text = shader_mm.read_text()

get_func_old = r'''	bool rehydrated = false;
	unique_lock<mutex> accessLock(_accessLock, defer_lock);
	if (_repository) { accessLock.lock(); }
	bool wasResident = isResident();
	if (_repository) {
		if (!ensureResidentLocked(allowLibraryCompile)) { return MVKMTLFunctionNull; }
	} else if (!_mtlLibrary) {
		return MVKMTLFunctionNull;
	}
	rehydrated = !wasResident;
	touch();
'''
get_func_new = r'''	bool rehydrated = false;
	uint64_t rehydrateStartedAt = 0;
	uint64_t rehydrateNanoseconds = 0;
	unique_lock<mutex> accessLock(_accessLock, defer_lock);
	if (_repository) { accessLock.lock(); }
	bool wasResident = isResident();
	if (_repository) {
		if (!wasResident) { rehydrateStartedAt = mvkGetTimestamp(); }
		if (!ensureResidentLocked(allowLibraryCompile)) {
			if (rehydrateStartedAt) {
				_repository->recordRehydrateFailure(
					mvkGetElapsedNanoseconds(rehydrateStartedAt));
			}
			return MVKMTLFunctionNull;
		}
	} else if (!_mtlLibrary) {
		return MVKMTLFunctionNull;
	}
	rehydrated = !wasResident;
	if (rehydrated && rehydrateStartedAt) {
		rehydrateNanoseconds = mvkGetElapsedNanoseconds(rehydrateStartedAt);
	}
	touch();
'''
if text.count(get_func_old) != 1:
    raise SystemExit("MVKShaderLibrary::getMTLFunction rehydrate block mismatch")
text = text.replace(get_func_old, get_func_new, 1)
text = text.replace(
    "if (rehydrated && _repository) { _repository->libraryBecameResident(this, true); }",
    "if (rehydrated && _repository) {\n"
    "\t\t_repository->libraryBecameResident(this, true, rehydrateNanoseconds);\n"
    "\t}",
    1,
)
text = text.replace(
    "if (_repository) { _repository->libraryBecameResident(this, false); }",
    "if (_repository) { _repository->libraryBecameResident(this, false, 0); }",
    1,
)

snapshot_insert = text.index("void MVKShaderLibrary::setEntryPointName")
snapshot_impl = r'''
bool MVKShaderLibrary::tryGetMemorySnapshot(MVKShaderLibraryMemorySnapshot& snapshot) {
    unique_lock<mutex> accessLock(_accessLock, try_to_lock);
    if (!accessLock.owns_lock()) { return false; }

    snapshot = {};
    auto addLibrary = [&snapshot](MVKShaderLibrary* library, bool specializationVariant) {
        if (!library) { return; }
        snapshot.shaderLibraryCount++;
        if (specializationVariant) { snapshot.specializationVariantCount++; }
        uint64_t compressedBytes = library->_compressedMSL._compressed.size();
        uint64_t uncompressedBytes = library->_compressedMSL._uncompressedSize;
        snapshot.compressedMSLBytes += compressedBytes;
        snapshot.uncompressedMSLBytes += uncompressedBytes;
        if (library->_mtlLibrary && library->isResident()) {
            snapshot.residentShaderLibraryCount++;
            snapshot.residentUncompressedMSLBytes += uncompressedBytes;
        }
        snapshot.estimatedHostBytes +=
            sizeof(MVKShaderLibrary) +
            library->_compressedMSL._compressed.capacity();
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
        snapshot.estimatedHostBytes += library->_metal4LibraryContentKey.capacity();
#endif
    };

    addLibrary(this, false);
    snapshot.estimatedHostBytes +=
        _specializationVariants.size() *
        (sizeof(decltype(_specializationVariants)::value_type) + (3 * sizeof(void*)));
    for (const auto& variant : _specializationVariants) {
        addLibrary(variant.second, true);
    }
    return true;
}

'''
text = text[:snapshot_insert] + snapshot_impl + text[snapshot_insert:]

repo_sig_old = r'''void MVKShaderLibraryRepository::libraryBecameResident(
	MVKShaderLibrary* library,
	bool rehydrated) {'''
repo_sig_new = r'''void MVKShaderLibraryRepository::libraryBecameResident(
	MVKShaderLibrary* library,
	bool rehydrated,
	uint64_t rehydrateNanoseconds) {'''
if text.count(repo_sig_old) != 1:
    raise SystemExit("libraryBecameResident signature mismatch")
text = text.replace(repo_sig_old, repo_sig_new, 1)
text = text.replace(
    "\t\t\tif (rehydrated) { _rehydrateCount.fetch_add(1, memory_order_relaxed); }",
    "\t\t\tif (rehydrated) {\n"
    "\t\t\t\t_rehydrateCount.fetch_add(1, memory_order_relaxed);\n"
    "\t\t\t\t_rehydrateTotalNanoseconds.fetch_add(rehydrateNanoseconds, memory_order_relaxed);\n"
    "\t\t\t\tupdateAtomicMaximum(_rehydrateMaximumNanoseconds, rehydrateNanoseconds);\n"
    "\t\t\t}",
    1,
)
record_failure_marker = "void MVKShaderLibraryRepository::libraryBecameCold(MVKShaderLibrary* library) {"
record_failure_impl = r'''void MVKShaderLibraryRepository::recordRehydrateFailure(uint64_t rehydrateNanoseconds) {
    _rehydrateFailureCount.fetch_add(1, memory_order_relaxed);
    _rehydrateTotalNanoseconds.fetch_add(rehydrateNanoseconds, memory_order_relaxed);
    updateAtomicMaximum(_rehydrateMaximumNanoseconds, rehydrateNanoseconds);
}

'''
text = text.replace(record_failure_marker, record_failure_impl + record_failure_marker, 1)
text = text.replace(
    "\t\t_residentEvictionCount.fetch_add(1, memory_order_relaxed);",
    "\t\t_residentEvictionCount.fetch_add(1, memory_order_relaxed);\n"
    "\t\t_evictedUncompressedMSLBytes.fetch_add(\n"
    "\t\t\tlibrary->_compressedMSL._uncompressedSize,\n"
    "\t\t\tmemory_order_relaxed);",
    1,
)

trim_start_old = r'''void MVKShaderLibraryRepository::trimToResidentLimit(MVKShaderLibrary* protectedLibrary) {
	if (_residentLimit == 0 ||
		_residentEntryCount.load(memory_order_relaxed) <= _residentTrimHighWater) {
		return;
	}

	unique_lock<mutex> trimLock(_trimLock, try_to_lock);
	if (!trimLock.owns_lock() ||
		_residentEntryCount.load(memory_order_relaxed) <= _residentTrimHighWater) {
		return;
	}
'''
trim_start_new = r'''void MVKShaderLibraryRepository::trimToResidentLimit(MVKShaderLibrary* protectedLibrary) {
	if (_residentLimit == 0 ||
		_residentEntryCount.load(memory_order_relaxed) <= _residentTrimHighWater) {
		return;
	}

	uint64_t trimStartedAt = mvkGetTimestamp();
	_trimCycleCount.fetch_add(1, memory_order_relaxed);
	unique_lock<mutex> trimLock(_trimLock, try_to_lock);
	if (!trimLock.owns_lock() ||
		_residentEntryCount.load(memory_order_relaxed) <= _residentTrimHighWater) {
		_trimBusyCount.fetch_add(1, memory_order_relaxed);
		uint64_t trimNanoseconds = mvkGetElapsedNanoseconds(trimStartedAt);
		_trimTotalNanoseconds.fetch_add(trimNanoseconds, memory_order_relaxed);
		updateAtomicMaximum(_trimMaximumNanoseconds, trimNanoseconds);
		return;
	}
'''
if text.count(trim_start_old) != 1:
    raise SystemExit("trim start mismatch")
text = text.replace(trim_start_old, trim_start_new, 1)
text = text.replace(
    "\tsort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {",
    "\t_trimCandidateCount.fetch_add(candidates.size(), memory_order_relaxed);\n\n"
    "\tsort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {",
    1,
)
text = text.replace(
    "\tfor (const Candidate& candidate : candidates) { candidate.library->release(); }\n}",
    "\tfor (const Candidate& candidate : candidates) { candidate.library->release(); }\n"
    "\tuint64_t trimNanoseconds = mvkGetElapsedNanoseconds(trimStartedAt);\n"
    "\t_trimTotalNanoseconds.fetch_add(trimNanoseconds, memory_order_relaxed);\n"
    "\tupdateAtomicMaximum(_trimMaximumNanoseconds, trimNanoseconds);\n"
    "}",
    1,
)

cache_ctor_marker = "MVKShaderLibraryCache::MVKShaderLibraryCache("
cache_stats_impl = r'''void MVKShaderLibraryCache::accumulateMemoryStatistics(
    MVKPipelineCacheMemoryStatistics* pStats) const {
    if (!pStats) { return; }

    pStats->estimatedViewHostBytes +=
        sizeof(*this) +
        (_shaderLibraries.capacity() * sizeof(decltype(_shaderLibraries)::value_type));
    for (const auto& libraryEntry : _shaderLibraries) {
        pStats->logicalShaderLibraryCount++;
        MVKShaderLibraryMemorySnapshot snapshot;
        if (!libraryEntry.second || !libraryEntry.second->tryGetMemorySnapshot(snapshot)) {
            pStats->skippedShaderLibraryCount++;
            continue;
        }
        pStats->logicalResidentShaderLibraryCount += snapshot.residentShaderLibraryCount;
        pStats->specializationVariantCount += snapshot.specializationVariantCount;
        pStats->compressedMSLBytes += snapshot.compressedMSLBytes;
        pStats->uncompressedMSLBytes += snapshot.uncompressedMSLBytes;
    }
}

'''
text = text.replace(cache_ctor_marker, cache_stats_impl + cache_ctor_marker, 1)

repo_stats_marker = "uint64_t MVKShaderLibraryRepository::nextUseSequence() {"
repo_stats_impl = r'''void MVKShaderLibraryRepository::getMemoryStatistics(
    MVKMetal4ShaderLibraryRepositoryStatistics* pStats) {
    if (!pStats) { return; }
    *pStats = {};

    vector<MVKShaderLibrary*> libraries;
    unique_lock<mutex> repositoryLock(_lock, try_to_lock);
    if (!repositoryLock.owns_lock()) { return; }

    pStats->available = VK_TRUE;
    pStats->logicalMembershipCount = _logicalMembershipCount.load(memory_order_relaxed);
    pStats->residentCanonicalShaderLibraryCount = _residentEntryCount.load(memory_order_relaxed);
    pStats->residentLimit = _residentLimit;
    pStats->residentTrimHighWater = _residentTrimHighWater;
    pStats->trimCycleCount = _trimCycleCount.load(memory_order_relaxed);
    pStats->trimBusyCount = _trimBusyCount.load(memory_order_relaxed);
    pStats->trimCandidateCount = _trimCandidateCount.load(memory_order_relaxed);
    pStats->trimTotalNanoseconds = _trimTotalNanoseconds.load(memory_order_relaxed);
    pStats->trimMaximumNanoseconds = _trimMaximumNanoseconds.load(memory_order_relaxed);
    pStats->residentEvictionCount = _residentEvictionCount.load(memory_order_relaxed);
    pStats->evictedUncompressedMSLBytes = _evictedUncompressedMSLBytes.load(memory_order_relaxed);
    pStats->rehydrateCount = _rehydrateCount.load(memory_order_relaxed);
    pStats->rehydrateFailureCount = _rehydrateFailureCount.load(memory_order_relaxed);
    pStats->rehydrateTotalNanoseconds = _rehydrateTotalNanoseconds.load(memory_order_relaxed);
    pStats->rehydrateMaximumNanoseconds = _rehydrateMaximumNanoseconds.load(memory_order_relaxed);
    pStats->dedupeHitCount = _dedupeHitCount.load(memory_order_relaxed);
    pStats->raceLoserCount = _raceLoserCount.load(memory_order_relaxed);
    pStats->residentAdoptionCount = _residentAdoptionCount.load(memory_order_relaxed);
    pStats->estimatedHostBytes =
        sizeof(*this) +
        (_entries.bucket_count() * sizeof(void*)) +
        (_entries.size() * sizeof(decltype(_entries)::value_type));

    for (auto& moduleEntries : _entries) {
        pStats->estimatedHostBytes +=
            moduleEntries.second.capacity() * sizeof(Entry);
        for (Entry& entry : moduleEntries.second) {
            pStats->canonicalShaderLibraryCount++;
            entry.library->retain();
            libraries.push_back(entry.library);
        }
    }
    repositoryLock.unlock();

    for (MVKShaderLibrary* library : libraries) {
        MVKShaderLibraryMemorySnapshot snapshot;
        if (library->tryGetMemorySnapshot(snapshot)) {
            pStats->residentShaderLibraryCount += snapshot.residentShaderLibraryCount;
            pStats->specializationVariantCount += snapshot.specializationVariantCount;
            pStats->compressedMSLBytes += snapshot.compressedMSLBytes;
            pStats->uncompressedMSLBytes += snapshot.uncompressedMSLBytes;
            pStats->residentUncompressedMSLBytes += snapshot.residentUncompressedMSLBytes;
            pStats->estimatedHostBytes += snapshot.estimatedHostBytes;
        } else {
            pStats->snapshotSkippedShaderLibraryCount++;
        }
        library->release();
    }

    if (@available(macOS 10.15, iOS 13.0, *)) {
        pStats->deviceCurrentAllocatedBytes = getMTLDevice().currentAllocatedSize;
    }
}

'''
text = text.replace(repo_stats_marker, repo_stats_impl + repo_stats_marker, 1)
shader_mm.write_text(text)

api_mm = Path("MoltenVK/MoltenVK/Vulkan/mvk_api.mm")
replace_once(
    api_mm,
    '#include "MVKShaderModule.h"\n',
    '#include "MVKShaderModule.h"\n#include "MVKPipeline.h"\n',
)
api_marker = r'''MVK_PUBLIC_VULKAN_SYMBOL VkResult vkGetPerformanceStatisticsMVK(
	VkDevice                                    device,
	MVKPerformanceStatistics*            		pPerf,
	size_t*                                     pPerfSize) {

	MVKPerformanceStatistics mvkPerf;
	MVKDevice::getMVKDevice(device)->getPerformanceStatistics(&mvkPerf);
	return mvkCopyGrowingStruct(pPerf, &mvkPerf, pPerfSize);
}
'''
api_addition = api_marker + r'''

MVK_PUBLIC_VULKAN_SYMBOL VkResult vkGetPipelineCacheMemoryStatisticsMVK(
    VkPipelineCache                           pipelineCache,
    MVKPipelineCacheMemoryStatistics*         pStats,
    size_t*                                   pStatsSize) {

    MVKPipelineCacheMemoryStatistics stats = {};
    if (pipelineCache) {
        ((MVKPipelineCache*)pipelineCache)->getMemoryStatistics(&stats);
    }
    return mvkCopyGrowingStruct(pStats, &stats, pStatsSize);
}

MVK_PUBLIC_VULKAN_SYMBOL VkResult vkGetMetal4ShaderLibraryRepositoryStatisticsMVK(
    VkDevice                                  device,
    MVKMetal4ShaderLibraryRepositoryStatistics* pStats,
    size_t*                                   pStatsSize) {

    MVKMetal4ShaderLibraryRepositoryStatistics stats = {};
    if (device) {
        MVKDevice* mvkDevice = MVKDevice::getMVKDevice(device);
        if (MVKShaderLibraryRepository* repository =
                mvkDevice->getShaderLibraryRepository()) {
            repository->getMemoryStatistics(&stats);
        }
    }
    return mvkCopyGrowingStruct(pStats, &stats, pStatsSize);
}
'''
replace_once(api_mm, api_marker, api_addition)

test = Path("Scripts/test-metal4-shared-shader-repository.py")
text = test.read_text()
anchor = '    require(shader_mm, "Metal 4 shared shader-library repository summary:", SHADER_MM)\n'
addition = anchor + r'''
    require(private_api_h, "MVKPipelineCacheMemoryStatistics", PRIVATE_API_H)
    require(private_api_h, "MVKMetal4ShaderLibraryRepositoryStatistics", PRIVATE_API_H)
    require(private_api_h, "vkGetPipelineCacheMemoryStatisticsMVK", PRIVATE_API_H)
    require(private_api_h, "vkGetMetal4ShaderLibraryRepositoryStatisticsMVK", PRIVATE_API_H)
    require(shader_h, "tryGetMemorySnapshot", SHADER_H)
    require(shader_h, "getMemoryStatistics(MVKMetal4ShaderLibraryRepositoryStatistics", SHADER_H)
    require(shader_mm, "rehydrateTotalNanoseconds", SHADER_MM)
    require(shader_mm, "evictedUncompressedMSLBytes", SHADER_MM)
    require(shader_mm, "deviceCurrentAllocatedBytes", SHADER_MM)
'''
if text.count(anchor) != 1:
    raise SystemExit("policy test anchor mismatch")
text = text.replace(anchor, addition, 1)
test.write_text(text)

doc = Path("Docs/Metal4_Shared_Shader_Library_Repository.md")
with doc.open("a") as handle:
    handle.write(r'''

## Runtime memory and reclaim telemetry

The private diagnostic ABI exposes two nonblocking snapshots:

- `vkGetPipelineCacheMemoryStatisticsMVK` reports the exact compressed and
  uncompressed MSL represented by one logical `VkPipelineCache` view plus a
  lower-bound estimate of that view's C++ index storage.
- `vkGetMetal4ShaderLibraryRepositoryStatisticsMVK` reports the unique physical
  canonical payload, logical membership count, resident/cold counts, reclaim
  proxy bytes, and rehydrate latency.

Metal does not expose the allocated byte size of an individual `MTLLibrary`.
`residentUncompressedMSLBytes` and `evictedUncompressedMSLBytes` are explicitly
workload-size proxies. `deviceCurrentAllocatedBytes` and process `phys_footprint`
remain the evidence for actual memory release. Snapshot acquisition uses
`try_lock`; a busy cache or library is reported as unavailable/skipped rather
than blocking a render or compiler thread.
''')

for path in [private_api, pipeline_h, shader_h, pipeline_mm, shader_mm, api_mm, test, doc]:
    if not path.exists():
        raise SystemExit(f"missing output: {path}")
