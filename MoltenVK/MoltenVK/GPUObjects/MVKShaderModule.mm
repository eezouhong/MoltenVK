/*
 * MVKShaderModule.mm
 *
 * Copyright (c) 2015-2026 The Brenwill Workshop Ltd. (http://www.brenwill.com)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MVKShaderModule.h"
#include "MVKPipeline.h"
#include "MVKFoundation.h"
#include <limits>
#include <sys/stat.h>

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
#include <CommonCrypto/CommonDigest.h>
#endif

using namespace std;
using namespace mvk;

static size_t getSharedShaderLibraryTrimHighWater(size_t residentLimit) {
	if (residentLimit == 0) { return 0; }
	size_t margin = max<size_t>(4, residentLimit / 16);
	return residentLimit > numeric_limits<size_t>::max() - margin
		? numeric_limits<size_t>::max()
		: residentLimit + margin;
}

template<typename T>
static void updateAtomicMaximum(atomic<T>& target, T value) {
	T current = target.load(memory_order_relaxed);
	while (current < value &&
		!target.compare_exchange_weak(
			current, value, memory_order_relaxed, memory_order_relaxed)) {}
}

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
static constexpr uint8_t kMetal4SourceLibraryContent = 1;
static constexpr uint8_t kMetal4CompiledLibraryContent = 2;

static void appendMetal4FunctionKeyBytes(string& key, const void* bytes, size_t size) {
	key.append(static_cast<const char*>(bytes), size);
}

template<typename T>
static void appendMetal4FunctionKeyValue(string& key, const T& value) {
	appendMetal4FunctionKeyBytes(key, &value, sizeof(value));
}

static bool updateMetal4LibraryContentDigest(CC_SHA256_CTX& context,
											 const void* bytes,
											 size_t size) {
	const uint8_t* cursor = static_cast<const uint8_t*>(bytes);
	while (size > 0) {
		CC_LONG chunkSize = static_cast<CC_LONG>(min<size_t>(
			size, numeric_limits<CC_LONG>::max()));
		if (!CC_SHA256_Update(&context, cursor, chunkSize)) { return false; }
		cursor += chunkSize;
		size -= chunkSize;
	}
	return true;
}

template<typename T>
static bool updateMetal4LibraryContentDigestValue(CC_SHA256_CTX& context, const T& value) {
	return updateMetal4LibraryContentDigest(context, &value, sizeof(value));
}

static string makeMetal4LibraryContentKey(
	const void* content,
	size_t contentSize,
	uint8_t contentKind,
	const SPIRVToMSLConversionResultInfo& resultInfo,
	const vector<pair<MSLSpecializationMacroInfo, MVKShaderMacroValue>>& macroDefinitions) {
	CC_SHA256_CTX context;
	if (!CC_SHA256_Init(&context)) { return {}; }
	static constexpr char kDomain[] = "MoltenVK.Metal4.LibraryContent.v1";
	const uint64_t stableContentSize = contentSize;
	uint32_t fpFastMathFlags = 0;
	uint8_t positionInvariant = 0;
	uint64_t macroCount = 0;
	if (contentKind == kMetal4SourceLibraryContent) {
		fpFastMathFlags = resultInfo.entryPoint.fpFastMathFlags;
		positionInvariant = resultInfo.isPositionInvariant ? 1 : 0;
		macroCount = macroDefinitions.size();
	} else if (contentKind != kMetal4CompiledLibraryContent) {
		return {};
	}
	if (!updateMetal4LibraryContentDigest(context, kDomain, sizeof(kDomain) - 1) ||
		!updateMetal4LibraryContentDigestValue(context, contentKind) ||
		!updateMetal4LibraryContentDigestValue(context, stableContentSize) ||
		!updateMetal4LibraryContentDigest(context, content, contentSize) ||
		!updateMetal4LibraryContentDigestValue(context, fpFastMathFlags) ||
		!updateMetal4LibraryContentDigestValue(context, positionInvariant) ||
		!updateMetal4LibraryContentDigestValue(context, macroCount)) {
		return {};
	}
	for (size_t macroIndex = 0; macroIndex < macroCount; macroIndex++) {
		const auto& macro = macroDefinitions[macroIndex];
		const uint64_t nameLength = macro.first.name.size();
		const uint8_t isFloat = macro.first.isFloat ? 1 : 0;
		const uint8_t isSigned = macro.first.isSigned ? 1 : 0;
		const uint64_t valueSize = min(macro.second.size, sizeof(macro.second.value));
		if (!updateMetal4LibraryContentDigestValue(context, nameLength) ||
			!updateMetal4LibraryContentDigest(context, macro.first.name.data(), macro.first.name.size()) ||
			!updateMetal4LibraryContentDigestValue(context, isFloat) ||
			!updateMetal4LibraryContentDigestValue(context, isSigned) ||
			!updateMetal4LibraryContentDigestValue(context, valueSize) ||
			!updateMetal4LibraryContentDigest(context, &macro.second.value, valueSize)) {
			return {};
		}
	}
	uint8_t digest[CC_SHA256_DIGEST_LENGTH];
	if (!CC_SHA256_Final(digest, &context)) { return {}; }
	return string(reinterpret_cast<const char*>(digest), sizeof(digest));
}

static void appendMetal4FunctionVariantKey(string& key,
										NSString* functionName,
										const VkSpecializationInfo* specializationInfo) {

	const char* utf8Name = functionName.UTF8String ?: "";
	size_t nameLength = strlen(utf8Name);
	appendMetal4FunctionKeyValue(key, nameLength);
	appendMetal4FunctionKeyBytes(key, utf8Name, nameLength);

	struct SpecializationEntry {
		uint32_t constantID;
		vector<uint8_t> value;
	};
	vector<SpecializationEntry> entries;
	if (specializationInfo) {
		entries.reserve(specializationInfo->mapEntryCount);
		for (uint32_t entryIdx = 0; entryIdx < specializationInfo->mapEntryCount; entryIdx++) {
			const VkSpecializationMapEntry& mapEntry = specializationInfo->pMapEntries[entryIdx];
			SpecializationEntry entry = { .constantID = mapEntry.constantID };
			entry.value.resize(mapEntry.size);
			memcpy(entry.value.data(), static_cast<const uint8_t*>(specializationInfo->pData) + mapEntry.offset, mapEntry.size);
			entries.emplace_back(std::move(entry));
		}
		std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
			if (left.constantID != right.constantID) { return left.constantID < right.constantID; }
			return left.value < right.value;
		});
	}
	appendMetal4FunctionKeyValue(key, entries.size());
	for (const auto& entry : entries) {
		appendMetal4FunctionKeyValue(key, entry.constantID);
		appendMetal4FunctionKeyValue(key, entry.value.size());
		appendMetal4FunctionKeyBytes(key, entry.value.data(), entry.value.size());
	}
	return;
}

static string makeMetal4FunctionKey(const string& libraryContentKey,
									NSString* functionName,
									const VkSpecializationInfo* specializationInfo) {
	if (libraryContentKey.empty()) { return {}; }
	string key;
	appendMetal4FunctionKeyValue(key, libraryContentKey.size());
	appendMetal4FunctionKeyBytes(key, libraryContentKey.data(), libraryContentKey.size());
	appendMetal4FunctionVariantKey(key, functionName, specializationInfo);
	return key;
}

static string makeMetal4PointerFunctionKey(id<MTLLibrary> library,
										   NSString* functionName,
										   const VkSpecializationInfo* specializationInfo) {
	string key;
	uintptr_t libraryIdentity = reinterpret_cast<uintptr_t>(library);
	appendMetal4FunctionKeyValue(key, libraryIdentity);
	appendMetal4FunctionVariantKey(key, functionName, specializationInfo);
	return key;
}
#endif

MVKMTLFunction::MVKMTLFunction(id<MTLFunction> mtlFunc,
								   const SPIRVToMSLConversionResultInfo scRslts,
								   MTLSize tgSize
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
								   , MTL4FunctionDescriptor* mtl4FuncDesc,
								   string mtl4FuncKey,
								   string mtl4PointerFuncKey
#endif
								   ) {
	_mtlFunction = [mtlFunc retain];		// retained
	shaderConversionResults = scRslts;
	threadGroupSize = tgSize;
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	_mtl4FunctionDescriptor = [mtl4FuncDesc retain];
	_mtl4FunctionKey = std::move(mtl4FuncKey);
	_mtl4PointerFunctionKey = std::move(mtl4PointerFuncKey);
#endif
}

MVKMTLFunction::MVKMTLFunction(const MVKMTLFunction& other) {
	_mtlFunction = [other._mtlFunction retain];		// retained
	shaderConversionResults = other.shaderConversionResults;
	threadGroupSize = other.threadGroupSize;
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	_mtl4FunctionDescriptor = [other._mtl4FunctionDescriptor retain];
	_mtl4FunctionKey = other._mtl4FunctionKey;
	_mtl4PointerFunctionKey = other._mtl4PointerFunctionKey;
#endif
}

MVKMTLFunction& MVKMTLFunction::operator=(const MVKMTLFunction& other) {
	// Retain new object first in case it's the same object
	[other._mtlFunction retain];
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	[other._mtl4FunctionDescriptor retain];
#endif
	[_mtlFunction release];
	_mtlFunction = other._mtlFunction;

	shaderConversionResults = other.shaderConversionResults;
	threadGroupSize = other.threadGroupSize;
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	[_mtl4FunctionDescriptor release];
	_mtl4FunctionDescriptor = other._mtl4FunctionDescriptor;
	_mtl4FunctionKey = other._mtl4FunctionKey;
	_mtl4PointerFunctionKey = other._mtl4PointerFunctionKey;
#endif
	return *this;
}

MVKMTLFunction::~MVKMTLFunction() {
	[_mtlFunction release];
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	[_mtl4FunctionDescriptor release];
#endif
}


#pragma mark -
#pragma mark MVKShaderLibrary

// If the size of the workgroup dimension is specialized, extract it from the
// specialization info, otherwise use the value specified in the SPIR-V shader code.
static uint32_t getWorkgroupDimensionSize(const SPIRVWorkgroupSizeDimension& wgDim, const VkSpecializationInfo* pSpecInfo) {
	if (wgDim.isSpecialized && pSpecInfo) {
		for (uint32_t specIdx = 0; specIdx < pSpecInfo->mapEntryCount; specIdx++) {
			const VkSpecializationMapEntry* pMapEntry = &pSpecInfo->pMapEntries[specIdx];
			if (pMapEntry->constantID == wgDim.specializationID) {
				return *reinterpret_cast<uint32_t*>((uintptr_t)pSpecInfo->pData + pMapEntry->offset) ;
			}
		}
	}
	return wgDim.size;
}

void MVKShaderLibrary::retain() {
	_referenceCount.fetch_add(1, memory_order_relaxed);
}

void MVKShaderLibrary::release() {
	uint32_t priorCount = _referenceCount.fetch_sub(1, memory_order_acq_rel);
	assert(priorCount > 0);
	if (priorCount == 1) { destroy(); }
}

MVKMTLFunction MVKShaderLibrary::getMTLFunction(const VkSpecializationInfo* pSpecializationInfo,
														VkPipelineCreationFeedback* pShaderFeedback,
														MVKShaderModule* shaderModule,
														bool allowLibraryCompile) {

	bool rehydrated = false;
	uint64_t rehydrateStartedAt = 0;
	uint64_t rehydrateNanoseconds = 0;
	unique_lock<mutex> accessLock(_accessLock, defer_lock);
	if (_repository) { accessLock.lock(); }
	bool wasResident = isResident();
	if (_repository) {
		if (!wasResident && allowLibraryCompile) { rehydrateStartedAt = mvkGetTimestamp(); }
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

	id<MTLLibrary> lib = _mtlLibrary;
	bool retainedLibraryForUnlockedUse = false;
	bool activeRepositoryUse = false;
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	const string* libraryContentKey = &_metal4LibraryContentKey;
	string retainedLibraryContentKey;
#endif

	// If specialization happens on constants mapped to macro, find or compile a library variant
	// with proper macro definition instead of the "generic" library
	if (pSpecializationInfo && _maySpecializeWithMacro) {
		// Create the list of macro-value mapping
		vector<pair<uint32_t, MVKShaderMacroValue>> spec_list;
		for (uint32_t specIdx = 0; specIdx < pSpecializationInfo->mapEntryCount; specIdx++) {
			const VkSpecializationMapEntry* pMapEntry = &pSpecializationInfo->pMapEntries[specIdx];
			uint32_t const_id = pMapEntry->constantID;
			MVKShaderMacroValue macro_value = {};
			size_t size = min(pMapEntry->size, sizeof(macro_value.value));

			memcpy(&macro_value.value, (char *)pSpecializationInfo->pData + pMapEntry->offset, size);
			macro_value.size = size;
			if (_shaderConversionResultInfo.specializationMacros.find(const_id) != _shaderConversionResultInfo.specializationMacros.end()) {
				spec_list.push_back(make_pair(const_id, macro_value));
			}
		}

		if (!spec_list.empty()) {
			// Sort the specialization list before it is used as a key to index the variants
			std::sort(spec_list.begin(), spec_list.end());
			auto entry = _specializationVariants.find(spec_list);
			if (entry != _specializationVariants.end()) {
				lib = entry->second->_mtlLibrary;
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
				libraryContentKey = &entry->second->getMetal4LibraryContentKey();
#endif
			} else {
				if (!allowLibraryCompile) { return MVKMTLFunctionNull; }
				MVKShaderLibrary *new_mvklib = new MVKShaderLibrary(_owner, _shaderConversionResultInfo, _compressedMSL, &spec_list);
				_specializationVariants[spec_list] = new_mvklib;
				lib = new_mvklib->_mtlLibrary;
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
				libraryContentKey = &new_mvklib->getMetal4LibraryContentKey();
#endif
			}
		}
	}

	// The repository lock protects canonical ownership, and the per-library
	// lock protects resident selection and macro-variant publication. Do not
	// hold either while asking Metal for a function: a background pipeline
	// compile using the same canonical shader must not serialize a foreground
	// pipeline on the full function-specialization path. A local Objective-C
	// retain keeps the selected library alive if reclaim runs after unlock.
	if (_repository) {
		if (lib) {
			[lib retain];
			retainedLibraryForUnlockedUse = true;
			_activeUseCount.fetch_add(1, memory_order_acq_rel);
			activeRepositoryUse = true;
		}
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
		retainedLibraryContentKey = *libraryContentKey;
		libraryContentKey = &retainedLibraryContentKey;
#endif
	}

	// A cold logical cache hit still required Metal compilation, so it is not
	// an application pipeline-cache hit for feedback purposes.
	if (rehydrated && pShaderFeedback) {
		mvkDisableFlags(
			pShaderFeedback->flags,
			VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT);
	}
	// Publish resident accounting while the per-library lock still protects
	// this freshly rehydrated payload. Otherwise a concurrent trim can see
	// a resident-but-uncounted entry between unlock and notification.
	if (rehydrated && _repository) {
		_repository->libraryBecameResident(this, true, rehydrateNanoseconds);
	}
	if (accessLock.owns_lock()) { accessLock.unlock(); }

	MVKMTLFunction result;
	@synchronized (getMTLDevice()) {
		@autoreleasepool {
			NSString* mtlFuncName = @(_shaderConversionResultInfo.entryPoint.mtlFunctionName.c_str());

			uint64_t startTime = pShaderFeedback ? mvkGetTimestamp() : getPerformanceTimestamp();
			id<MTLFunction> mtlFunc = [[lib newFunctionWithName: mtlFuncName] autorelease];
			addPerformanceInterval(getPerformanceStats().shaderCompilation.functionRetrieval, startTime);
			if (pShaderFeedback) {
				if (mtlFunc) {
					mvkEnableFlags(pShaderFeedback->flags, VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT);
				}
				pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime);
			}

			MTLFunctionConstantValues* mtlFCVals = nil;
			if (mtlFunc) {
				// If the Metal function expects to be specialized, populate Metal function constant values from
				// the Vulkan specialization info, and compile a specialized Metal function, otherwise simply use
				// the unspecialized Metal function.
				NSArray<MTLFunctionConstant*>* mtlFCs = mtlFunc.functionConstantsDictionary.allValues;
				if (mtlFCs.count > 0) {
					// The Metal shader contains function constants and expects to be specialized.
					// Populate Metal function constant values from Vulkan specialization data.
					mtlFCVals = [[MTLFunctionConstantValues new] autorelease];
					if (pSpecializationInfo) {
						for (uint32_t specIdx = 0; specIdx < pSpecializationInfo->mapEntryCount; specIdx++) {
							const VkSpecializationMapEntry* pMapEntry = &pSpecializationInfo->pMapEntries[specIdx];
							for (MTLFunctionConstant* mfc in mtlFCs) {
								if (mfc.index == pMapEntry->constantID) {
									[mtlFCVals setConstantValue: ((char*)pSpecializationInfo->pData + pMapEntry->offset)
														   type: mfc.type
														atIndex: mfc.index];
									break;
								}
							}
						}
					}

					MVKFunctionSpecializer fs(_owner);
					if (pShaderFeedback) { startTime = mvkGetTimestamp(); }
					mtlFunc = [fs.newMTLFunction(lib, mtlFuncName, mtlFCVals) autorelease];
					if (pShaderFeedback) { pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime); }
				}
			}

			NSString* dbName = shaderModule->getDebugName();
			if ( !dbName ) { dbName = _owner->getDebugName(); }
			_owner->setMetalObjectLabel(mtlFunc, dbName);

			auto& wgSize = _shaderConversionResultInfo.entryPoint.workgroupSize;
			MTLSize threadGroupSize = MTLSizeMake(getWorkgroupDimensionSize(wgSize.width, pSpecializationInfo),
														  getWorkgroupDimensionSize(wgSize.height, pSpecializationInfo),
														  getWorkgroupDimensionSize(wgSize.depth, pSpecializationInfo));
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
			MTL4FunctionDescriptor* mtl4FunctionDescriptor = nil;
			string mtl4FunctionKey;
			string mtl4PointerFunctionKey;
			if (mtlFunc && getDevice()->isMetal4FlexiblePipelineEnabled()) {
				if (@available(macOS 26.0, iOS 26.0, *)) {
					MTL4LibraryFunctionDescriptor* libraryDescriptor = [MTL4LibraryFunctionDescriptor new];
					libraryDescriptor.library = lib;
					libraryDescriptor.name = mtlFuncName;
					if (mtlFCVals) {
						MTL4SpecializedFunctionDescriptor* specializedDescriptor = [MTL4SpecializedFunctionDescriptor new];
						specializedDescriptor.functionDescriptor = libraryDescriptor;
						specializedDescriptor.constantValues = mtlFCVals;
						mtl4FunctionDescriptor = specializedDescriptor;
						[libraryDescriptor release];
					} else {
						mtl4FunctionDescriptor = libraryDescriptor;
					}
					mtl4FunctionKey = makeMetal4FunctionKey(*libraryContentKey, mtlFuncName, pSpecializationInfo);
					mtl4PointerFunctionKey = makeMetal4PointerFunctionKey(lib, mtlFuncName, pSpecializationInfo);
				}
			}
			result = MVKMTLFunction(mtlFunc,
										_shaderConversionResultInfo,
										threadGroupSize,
										mtl4FunctionDescriptor,
										std::move(mtl4FunctionKey),
										std::move(mtl4PointerFunctionKey));
			[mtl4FunctionDescriptor release];
#else
			result = MVKMTLFunction(mtlFunc, _shaderConversionResultInfo, threadGroupSize);
#endif
		}
	}

	if (retainedLibraryForUnlockedUse) { [lib release]; }
	if (activeRepositoryUse) {
		uint32_t priorUseCount = _activeUseCount.fetch_sub(1, memory_order_acq_rel);
		assert(priorUseCount > 0);
	}
	// Candidate selection and Metal function production are complete. Any
	// bounded resident-budget work happens outside the per-library lock.
	if (rehydrated && _repository) { _repository->trimToResidentLimit(this); }
	return result;
}


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

void MVKShaderLibrary::setEntryPointName(string& funcName) {
	_shaderConversionResultInfo.entryPoint.mtlFunctionName = funcName;
}

void MVKShaderLibrary::setWorkgroupSize(uint32_t x, uint32_t y, uint32_t z) {
	auto& wgSize = _shaderConversionResultInfo.entryPoint.workgroupSize;
	wgSize.width.size = x;
	wgSize.height.size = y;
	wgSize.depth.size = z;
}

// Sets the cached MSL source code, after first compressing it.
void MVKShaderLibrary::compressMSL(const string& msl) {
	uint64_t startTime = getPerformanceTimestamp();
	_compressedMSL.compress(msl, getMVKConfig().shaderSourceCompressionAlgorithm);
	addPerformanceInterval(getPerformanceStats().shaderCompilation.mslCompress, startTime);
}

// Decompresses the cached MSL into the string.
void MVKShaderLibrary::decompressMSL(string& msl) {
	uint64_t startTime = getPerformanceTimestamp();
	_compressedMSL.decompress(msl);
	addPerformanceInterval(getPerformanceStats().shaderCompilation.mslDecompress, startTime);
}

bool MVKShaderLibrary::ensureResidentLocked(bool allowLibraryCompile) {
	if (_mtlLibrary) {
		_resident.store(true, memory_order_release);
		return true;
	}
	if (!allowLibraryCompile) { return false; }

	string msl;
	decompressMSL(msl);
	if (msl.empty()) { return false; }
	compileLibrary(msl);
	return _mtlLibrary != nil;
}

void MVKShaderLibrary::touch() {
	if (_repository) {
		_lastUseSequence.store(_repository->nextUseSequence(), memory_order_relaxed);
	}
}

bool MVKShaderLibrary::tryGetEvictionSnapshot(MVKShaderLibraryEvictionSnapshot& snapshot) {
	unique_lock<mutex> accessLock(_accessLock, try_to_lock);
	if (!accessLock.owns_lock() ||
		!_mtlLibrary ||
		!isResident() ||
		_activeUseCount.load(memory_order_acquire) != 0) {
		return false;
	}

	snapshot = {};
	snapshot.lastUseSequence = getLastUseSequence();
	snapshot.lastRehydrateNanoseconds =
		_lastRehydrateNanoseconds.load(memory_order_relaxed);
	snapshot.rehydrateProtectedUntilSequence =
		_rehydrateProtectedUntilSequence.load(memory_order_relaxed);
	snapshot.reclaimableUncompressedMSLBytes =
		_compressedMSL._uncompressedSize;
	for (const auto& item : _specializationVariants) {
		if (item.second) {
			snapshot.reclaimableUncompressedMSLBytes +=
				item.second->_compressedMSL._uncompressedSize;
		}
	}
	return true;
}

bool MVKShaderLibrary::tryAdoptResidentPayload(MVKShaderLibrary* candidate) {
	if (!candidate ||
		candidate == this ||
		candidate->_repository ||
		!candidate->isResident() ||
		!candidate->_mtlLibrary) {
		return false;
	}

	unique_lock<mutex> accessLock(_accessLock);
	if (_mtlLibrary ||
		isResident() ||
		!_repositoryTracked.load(memory_order_acquire) ||
		_activeUseCount.load(memory_order_acquire) != 0 ||
		!candidate->_mtlLibrary) {
		return false;
	}

	// Transfer the candidate's retained MTLLibrary reference instead of
	// discarding it and recompiling the cold canonical entry. Fresh candidates
	// have no published users, so their payload can move without another lock.
	_mtlLibrary = candidate->_mtlLibrary;
	candidate->_mtlLibrary = nil;
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	_metal4LibraryContentKey = candidate->_metal4LibraryContentKey;
	candidate->_metal4LibraryContentKey.clear();
#endif
	_resident.store(true, memory_order_release);
	candidate->_resident.store(false, memory_order_release);
	if (_repository) { _repository->libraryBecameResident(this, false, 0); }
	return true;
}

bool MVKShaderLibrary::tryEvictResident(
	const MVKShaderLibraryEvictionSnapshot& snapshot,
	uint64_t currentUseSequence) {
	unique_lock<mutex> accessLock(_accessLock, try_to_lock);
	if (!accessLock.owns_lock() ||
		!_mtlLibrary ||
		!isResident() ||
		_activeUseCount.load(memory_order_acquire) != 0 ||
		getLastUseSequence() != snapshot.lastUseSequence ||
		_rehydrateProtectedUntilSequence.load(memory_order_relaxed) !=
			snapshot.rehydrateProtectedUntilSequence ||
		currentUseSequence < snapshot.rehydrateProtectedUntilSequence) {
		return false;
	}

	uint64_t evictedUncompressedMSLBytes =
		_compressedMSL._uncompressedSize;
	for (const auto& item : _specializationVariants) {
		if (item.second) {
			evictedUncompressedMSLBytes +=
				item.second->_compressedMSL._uncompressedSize;
		}
	}

	id<MTLLibrary> releasedLibrary = _mtlLibrary;
	_mtlLibrary = nil;
	map<vector<pair<uint32_t, MVKShaderMacroValue>>, MVKShaderLibrary*> releasedVariants;
	releasedVariants.swap(_specializationVariants);
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	_metal4LibraryContentKey.clear();
#endif
	_resident.store(false, memory_order_release);
	if (_repository) {
		_repository->libraryBecameCold(
			this,
			evictedUncompressedMSLBytes);
	}
	accessLock.unlock();

	[releasedLibrary release];
	for (auto& item : releasedVariants) { item.second->release(); }
	return true;
}

MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,
								   const SPIRVToMSLConversionResult& conversionResult) :
	MVKBaseDeviceObject(owner->getDevice()),
	_owner(owner),
	_maySpecializeWithMacro(true) {

	_shaderConversionResultInfo = conversionResult.resultInfo;
	compressMSL(conversionResult.msl);
	compileLibrary(conversionResult.msl);
}

MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,
								   const SPIRVToMSLConversionResultInfo& resultInfo,
								   const MVKCompressor<std::string> compressedMSL,
								   const vector<pair<uint32_t, MVKShaderMacroValue> >* specializationMacroDef) :
	MVKBaseDeviceObject(owner->getDevice()),
	_owner(owner),
	_maySpecializeWithMacro(specializationMacroDef == nullptr) {

	_shaderConversionResultInfo = resultInfo;
	_compressedMSL = compressedMSL;
	string msl;
	decompressMSL(msl);
	compileLibrary(msl, specializationMacroDef);
}

void MVKShaderLibrary::compileLibrary(const string& msl,
									  const vector<pair<uint32_t, MVKShaderMacroValue> >* specializationMacroDef) {
	assert(!_mtlLibrary);
	MVKShaderLibraryCompiler* slc = new MVKShaderLibraryCompiler(_owner);
	NSString* nsSrc = [[NSString alloc] initWithUTF8String: msl.c_str()];	// temp retained

	// If specialization macro is used, translate the id to macro information and pass it to compiler
	vector<pair<MSLSpecializationMacroInfo, MVKShaderMacroValue>> macro_def;
	if (specializationMacroDef) {
		for (auto& def: *specializationMacroDef) {
			const auto& macro_name_iter = _shaderConversionResultInfo.specializationMacros.find(def.first);
			if (macro_name_iter != _shaderConversionResultInfo.specializationMacros.end()) {
				macro_def.push_back(make_pair(macro_name_iter->second, def.second));
			}
		}
	}

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	_metal4LibraryContentKey = makeMetal4LibraryContentKey(msl.data(),
														 msl.size(),
														 kMetal4SourceLibraryContent,
														 _shaderConversionResultInfo,
														 macro_def);
#endif
	_mtlLibrary = slc->newMTLLibrary(nsSrc, _shaderConversionResultInfo, macro_def);	// retained
	[nsSrc release];														// release temp string
	slc->destroy();
	_resident.store(_mtlLibrary != nil, memory_order_release);
}

MVKShaderLibrary::MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,
                                   const void* mslCompiledCodeData,
                                   size_t mslCompiledCodeLength) :
	MVKBaseDeviceObject(owner->getDevice()),
	_owner(owner),
	_maySpecializeWithMacro(false) {

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	_metal4LibraryContentKey = makeMetal4LibraryContentKey(mslCompiledCodeData,
														 mslCompiledCodeLength,
														 kMetal4CompiledLibraryContent,
														 _shaderConversionResultInfo,
														 {});
#endif
	uint64_t startTime = getPerformanceTimestamp();
    @autoreleasepool {
        dispatch_data_t shdrData = dispatch_data_create(mslCompiledCodeData,
                                                        mslCompiledCodeLength,
                                                        NULL,
                                                        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError* err = nil;
        _mtlLibrary = [getMTLDevice() newLibraryWithData: shdrData error: &err];    // retained
        handleCompilationError(err, "Compiled shader module creation");
        [shdrData release];
    }
	addPerformanceInterval(getPerformanceStats().shaderCompilation.mslLoad, startTime);
	_resident.store(_mtlLibrary != nil, memory_order_release);
}

MVKShaderLibrary::MVKShaderLibrary(const MVKShaderLibrary& other) :
	MVKBaseDeviceObject(other._device),
	_owner(other._owner),
	_maySpecializeWithMacro(other._maySpecializeWithMacro),
	_specializationVariants(other._specializationVariants) {

	_mtlLibrary = [other._mtlLibrary retain];
	_resident.store(_mtlLibrary != nil, memory_order_release);
	for (auto& item : _specializationVariants) { item.second->retain(); }
	_shaderConversionResultInfo = other._shaderConversionResultInfo;
	_compressedMSL = other._compressedMSL;
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	_metal4LibraryContentKey = other._metal4LibraryContentKey;
#endif
}

MVKShaderLibrary& MVKShaderLibrary::operator=(const MVKShaderLibrary& other) {
	if (this == &other) { return *this; }
	if (_mtlLibrary != other._mtlLibrary) {
		[_mtlLibrary release];
		_mtlLibrary = [other._mtlLibrary retain];
	}
	for (auto& item : _specializationVariants) { item.second->release(); }
	_specializationVariants = other._specializationVariants;
	for (auto& item : _specializationVariants) { item.second->retain(); }
	_owner = other._owner;
	_maySpecializeWithMacro = other._maySpecializeWithMacro;
	_resident.store(_mtlLibrary != nil, memory_order_release);
	_shaderConversionResultInfo = other._shaderConversionResultInfo;
	_compressedMSL = other._compressedMSL;
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	_metal4LibraryContentKey = other._metal4LibraryContentKey;
#endif
	return *this;
}

// If err object is nil, the compilation succeeded without any warnings.
// If err object exists, and the MTLLibrary was created, the compilation succeeded, but with warnings.
// If err object exists, and the MTLLibrary was not created, the compilation failed.
void MVKShaderLibrary::handleCompilationError(NSError* err, const char* opDesc) {
    if ( !err ) return;

    if (_mtlLibrary) {
        MVKLogInfo("%s succeeded with warnings (Error code %li):\n%s", opDesc, (long)err.code, err.localizedDescription.UTF8String);
    } else {
		VkResult error = reportError(VK_ERROR_INITIALIZATION_FAILED,
												   "%s failed (Error code %li):\n%s",
												   opDesc, (long)err.code,
												   err.localizedDescription.UTF8String);
		if (!_repository) { _owner->setConfigurationResult(error); }
    }
}

MVKShaderLibrary::~MVKShaderLibrary() {
	_resident.store(false, memory_order_release);
	[_mtlLibrary release];

	for (auto& item: _specializationVariants) {
		item.second->release();
	}
}


#pragma mark -
#pragma mark MVKShaderLibraryRepository

MVKShaderLibraryRepository* MVKShaderLibraryRepository::create(MVKDevice* device) {
    const MVKConfiguration& config = device->getMVKConfig();
    return config.metal4SharedShaderLibraryRepositoryEnabled
        ? new MVKShaderLibraryRepository(
            device,
            config.metal4SharedShaderLibraryResidentLimit)
        : nullptr;
}

MVKShaderLibraryRepository::MVKShaderLibraryRepository(
	MVKDevice* device,
	size_t residentLimit) :
	MVKVulkanAPIDeviceObject(device),
	_residentLimit(residentLimit),
	_residentTrimHighWater(getSharedShaderLibraryTrimHighWater(residentLimit)) {
	if (_residentLimit == 0) {
		reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
					  "Metal 4 shared shader-library repository enabled: deduplication only.");
	} else {
		reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,
					  "Metal 4 shared shader-library resident limit: %zu (trim high-water: %zu).",
					  _residentLimit,
					  _residentTrimHighWater);
	}
}

MVKShaderLibraryRepository::~MVKShaderLibraryRepository() {
	vector<MVKShaderLibrary*> libraries;
	size_t remainingCanonicalEntries = 0;
	uint64_t remainingMemberships = 0;
	{
		lock_guard<mutex> lock(_lock);
		for (auto& moduleEntries : _entries) {
			for (Entry& entry : moduleEntries.second) {
				remainingCanonicalEntries++;
				remainingMemberships += entry.membershipCount;
				entry.library->_repositoryTracked.store(false, memory_order_release);
				untrackResident(entry.library);
				libraries.push_back(entry.library);
			}
		}
		_entries.clear();
	}

	reportMessage(
		MVK_CONFIG_LOG_LEVEL_INFO,
		"Metal 4 shared shader-library repository summary: published=%llu, dedupeHits=%llu, raceLosers=%llu, residentPeak=%zu, residentEvictions=%llu, residentAdoptions=%llu, rehydrates=%llu, membershipPeak=%llu, remainingCanonical=%zu, remainingMemberships=%llu.",
		static_cast<unsigned long long>(_canonicalPublishCount.load(memory_order_relaxed)),
		static_cast<unsigned long long>(_dedupeHitCount.load(memory_order_relaxed)),
		static_cast<unsigned long long>(_raceLoserCount.load(memory_order_relaxed)),
		_residentPeakCount.load(memory_order_relaxed),
		static_cast<unsigned long long>(_residentEvictionCount.load(memory_order_relaxed)),
		static_cast<unsigned long long>(_residentAdoptionCount.load(memory_order_relaxed)),
		static_cast<unsigned long long>(_rehydrateCount.load(memory_order_relaxed)),
		static_cast<unsigned long long>(_logicalMembershipPeak.load(memory_order_relaxed)),
		remainingCanonicalEntries,
		static_cast<unsigned long long>(remainingMemberships));

	// Release only the repository ownership references. Valid Vulkan object
	// lifetime requires all pipeline-cache membership references to be gone first.
	for (MVKShaderLibrary* library : libraries) { library->release(); }
}

void MVKShaderLibraryRepository::getMemoryStatistics(
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
    pStats->costAwareCandidateCount = _costAwareCandidateCount.load(memory_order_relaxed);
    pStats->costAwareEvictionCount = _costAwareEvictionCount.load(memory_order_relaxed);
    pStats->rehydrateProtectionSkipCount = _rehydrateProtectionSkipCount.load(memory_order_relaxed);
    pStats->unknownRehydrateCostCandidateCount = _unknownRehydrateCostCandidateCount.load(memory_order_relaxed);
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

uint64_t MVKShaderLibraryRepository::nextUseSequence() {
	return _nextUseSequence.fetch_add(1, memory_order_relaxed) + 1;
}

void MVKShaderLibraryRepository::untrackResident(MVKShaderLibrary* library) {
	if (!library) { return; }
	bool expected = true;
	if (library->_repositoryResidentCounted.compare_exchange_strong(
			expected, false, memory_order_acq_rel)) {
		size_t priorCount = _residentEntryCount.fetch_sub(1, memory_order_acq_rel);
		assert(priorCount > 0);
	}
}

void MVKShaderLibraryRepository::libraryBecameResident(
	MVKShaderLibrary* library,
	bool rehydrated,
	uint64_t rehydrateNanoseconds) {

	if (!library || library->_repository != this || !library->isResident()) { return; }
	{
		lock_guard<mutex> lock(_lock);
		uint64_t currentUseSequence =
			_nextUseSequence.load(memory_order_relaxed);
		library->_rehydrateProtectedUntilSequence.store(
			currentUseSequence + 256,
			memory_order_relaxed);
		if (rehydrated && rehydrateNanoseconds != 0) {
			library->_lastRehydrateNanoseconds.store(
				rehydrateNanoseconds,
				memory_order_relaxed);
		}
		if (!library->_repositoryTracked.load(memory_order_acquire)) { return; }
		bool expected = false;
		if (library->_repositoryResidentCounted.compare_exchange_strong(
				expected, true, memory_order_acq_rel)) {
			size_t residentCount =
				_residentEntryCount.fetch_add(1, memory_order_relaxed) + 1;
			updateAtomicMaximum(_residentPeakCount, residentCount);
			if (rehydrated) {
				_rehydrateCount.fetch_add(1, memory_order_relaxed);
				_rehydrateTotalNanoseconds.fetch_add(rehydrateNanoseconds, memory_order_relaxed);
				updateAtomicMaximum(_rehydrateMaximumNanoseconds, rehydrateNanoseconds);
			}
		}
	}
}

void MVKShaderLibraryRepository::recordRehydrateFailure(uint64_t rehydrateNanoseconds) {
    _rehydrateFailureCount.fetch_add(1, memory_order_relaxed);
    _rehydrateTotalNanoseconds.fetch_add(rehydrateNanoseconds, memory_order_relaxed);
    updateAtomicMaximum(_rehydrateMaximumNanoseconds, rehydrateNanoseconds);
}

void MVKShaderLibraryRepository::libraryBecameCold(
	MVKShaderLibrary* library,
	uint64_t evictedUncompressedMSLBytes) {
	if (!library || library->_repository != this) { return; }
	bool expected = true;
	if (library->_repositoryResidentCounted.compare_exchange_strong(
			expected, false, memory_order_acq_rel)) {
		size_t priorCount = _residentEntryCount.fetch_sub(1, memory_order_acq_rel);
		assert(priorCount > 0);
		_residentEvictionCount.fetch_add(1, memory_order_relaxed);
		_evictedUncompressedMSLBytes.fetch_add(
			evictedUncompressedMSLBytes,
			memory_order_relaxed);
	}
}

void MVKShaderLibraryRepository::trimToResidentLimit(MVKShaderLibrary* protectedLibrary) {
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

	constexpr uint64_t kUnknownRehydrateCostNanoseconds = 16'000'000;
	uint64_t currentUseSequence = _nextUseSequence.load(memory_order_relaxed);
	struct Candidate {
		uint64_t score;
		MVKShaderLibraryEvictionSnapshot snapshot;
		MVKShaderLibrary* library;
	};
	vector<MVKShaderLibrary*> retainedLibraries;
	{
		lock_guard<mutex> lock(_lock);
		for (auto& moduleEntries : _entries) {
			for (Entry& entry : moduleEntries.second) {
				MVKShaderLibrary* library = entry.library;
				if (library == protectedLibrary ||
					!library->isResident() ||
					!library->_repositoryResidentCounted.load(memory_order_acquire)) {
					continue;
				}
				library->retain();
				retainedLibraries.push_back(library);
			}
		}
	}

	vector<Candidate> candidates;
	candidates.reserve(retainedLibraries.size());
	for (MVKShaderLibrary* library : retainedLibraries) {
		MVKShaderLibraryEvictionSnapshot snapshot;
		if (!library->tryGetEvictionSnapshot(snapshot)) { continue; }
		if (currentUseSequence < snapshot.rehydrateProtectedUntilSequence) {
			_rehydrateProtectionSkipCount.fetch_add(1, memory_order_relaxed);
			continue;
		}
		uint64_t coldness = currentUseSequence >= snapshot.lastUseSequence
			? currentUseSequence - snapshot.lastUseSequence : 0;
		uint64_t benefitPages = min<uint64_t>(snapshot.reclaimableUncompressedMSLBytes / 4096, 65535);
		uint64_t rehydrateCost = snapshot.lastRehydrateNanoseconds;
		if (rehydrateCost == 0) {
			rehydrateCost = kUnknownRehydrateCostNanoseconds;
			_unknownRehydrateCostCandidateCount.fetch_add(1, memory_order_relaxed);
		}
		uint64_t costMicros = min<uint64_t>(rehydrateCost / 1000, 65535);
		uint64_t score = (min<uint64_t>(coldness, 65535) * 4) + (benefitPages * 2) + (65535 - costMicros);
		candidates.push_back({ score, snapshot, library });
	}

	_trimCandidateCount.fetch_add(candidates.size(), memory_order_relaxed);
	_costAwareCandidateCount.fetch_add(candidates.size(), memory_order_relaxed);

	sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
		if (left.score != right.score) { return left.score > right.score; }
		if (left.snapshot.lastUseSequence != right.snapshot.lastUseSequence) {
			return left.snapshot.lastUseSequence < right.snapshot.lastUseSequence;
		}
		return left.snapshot.reclaimableUncompressedMSLBytes >
			right.snapshot.reclaimableUncompressedMSLBytes;
	});

	for (const Candidate& candidate : candidates) {
		if (_residentEntryCount.load(memory_order_relaxed) <= _residentLimit) { break; }
		if (candidate.library->tryEvictResident(candidate.snapshot, currentUseSequence)) {
			_costAwareEvictionCount.fetch_add(1, memory_order_relaxed);
		}
	}

	for (MVKShaderLibrary* library : retainedLibraries) { library->release(); }
	uint64_t trimNanoseconds = mvkGetElapsedNanoseconds(trimStartedAt);
	_trimTotalNanoseconds.fetch_add(trimNanoseconds, memory_order_relaxed);
	updateAtomicMaximum(_trimMaximumNanoseconds, trimNanoseconds);
}

MVKShaderLibrary* MVKShaderLibraryRepository::acquire(
	MVKShaderModuleKey shaderModuleKey,
	SPIRVToMSLConversionConfiguration* pShaderConfig,
	MVKShaderLibrary* candidate) {

	if (!pShaderConfig) {
		if (candidate) { candidate->release(); }
		return nullptr;
	}

	MVKShaderLibrary* result = nullptr;
	MVKShaderLibrary* rejectedCandidate = candidate;
	bool publishedCandidate = false;
	bool shouldAdoptCandidatePayload = false;
	bool adoptedCandidatePayload = false;
	{
		lock_guard<mutex> lock(_lock);
		auto moduleIt = _entries.find(shaderModuleKey);
		if (moduleIt != _entries.end()) {
			for (Entry& entry : moduleIt->second) {
				if (entry.shaderConfig.matches(*pShaderConfig)) {
					pShaderConfig->alignWith(entry.shaderConfig);
					entry.membershipCount++;
					entry.library->retain();
					result = entry.library;
					shouldAdoptCandidatePayload =
						candidate &&
						candidate->isResident() &&
						!entry.library->isResident();
					uint64_t membershipCount =
						_logicalMembershipCount.fetch_add(1, memory_order_relaxed) + 1;
					updateAtomicMaximum(_logicalMembershipPeak, membershipCount);
					_dedupeHitCount.fetch_add(1, memory_order_relaxed);
					break;
				}
			}
		}

		if (!result && candidate && candidate->isResident()) {
			// Only a successfully materialized candidate may become canonical.
			// A failed import/compile remains a local failure and is released
			// after the repository lock instead of poisoning future cache views.
			// The candidate's initial reference becomes repository ownership.
			// Add one separate reference for the acquiring cache view.
			candidate->_owner = this;
			candidate->_repository = this;
			candidate->_repositoryTracked.store(true, memory_order_release);
			candidate->retain();
			_entries[shaderModuleKey].push_back({ *pShaderConfig, candidate, 1 });
			_canonicalPublishCount.fetch_add(1, memory_order_relaxed);
			uint64_t membershipCount =
				_logicalMembershipCount.fetch_add(1, memory_order_relaxed) + 1;
			updateAtomicMaximum(_logicalMembershipPeak, membershipCount);
			if (candidate->isResident()) {
				bool expected = false;
				if (candidate->_repositoryResidentCounted.compare_exchange_strong(
						expected, true, memory_order_acq_rel)) {
					size_t residentCount =
						_residentEntryCount.fetch_add(1, memory_order_relaxed) + 1;
					updateAtomicMaximum(_residentPeakCount, residentCount);
				}
			}
			result = candidate;
			rejectedCandidate = nullptr;
			publishedCandidate = true;
		}
	}

	if (shouldAdoptCandidatePayload && result && candidate) {
		adoptedCandidatePayload = result->tryAdoptResidentPayload(candidate);
		if (adoptedCandidatePayload) {
			_residentAdoptionCount.fetch_add(1, memory_order_relaxed);
		}
	}

	if (publishedCandidate || adoptedCandidatePayload) {
		trimToResidentLimit(result);
	}

	// A competing path published first, or this candidate failed to
	// materialize. Drop it outside the repository lock. An adopted
	// candidate has already transferred its retained Metal payload.
	if (rejectedCandidate) {
		if (result && !adoptedCandidatePayload) {
			_raceLoserCount.fetch_add(1, memory_order_relaxed);
		}
		rejectedCandidate->release();
	}
	return result;
}

void MVKShaderLibraryRepository::release(
	MVKShaderModuleKey shaderModuleKey,
	const SPIRVToMSLConversionConfiguration& shaderConfig,
	MVKShaderLibrary* library) {

	if (!library) { return; }

	bool releaseRepositoryReference = false;
	bool found = false;
	{
		lock_guard<mutex> lock(_lock);
		auto moduleIt = _entries.find(shaderModuleKey);
		if (moduleIt != _entries.end()) {
			auto& entries = moduleIt->second;
			for (auto entryIt = entries.begin(); entryIt != entries.end(); ++entryIt) {
				if (entryIt->library == library && entryIt->shaderConfig.matches(shaderConfig)) {
					found = true;
					assert(entryIt->membershipCount > 0);
					entryIt->membershipCount--;
					if (entryIt->membershipCount == 0) {
						library->_repositoryTracked.store(false, memory_order_release);
						untrackResident(library);
						entries.erase(entryIt);
						releaseRepositoryReference = true;
						if (entries.empty()) { _entries.erase(moduleIt); }
					}
					break;
				}
			}
		}
	}

	if (!found) {
		reportMessage(MVK_CONFIG_LOG_LEVEL_WARNING,
					  "Shared shader-library membership was not present during release.");
		return;
	}

	uint64_t priorMembershipCount =
		_logicalMembershipCount.fetch_sub(1, memory_order_acq_rel);
	assert(priorMembershipCount > 0);

	// Release references only after the exact logical membership was found.
	// A mismatch must not underflow or destroy a canonical physical entry.
	library->release();
	if (releaseRepositoryReference) { library->release(); }
}


#pragma mark -
#pragma mark MVKShaderLibraryCache

void MVKShaderLibraryCache::accumulateMemoryStatistics(
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

MVKShaderLibraryCache::MVKShaderLibraryCache(
	MVKVulkanAPIDeviceObject* owner,
	MVKShaderModuleKey shaderModuleKey) :
	MVKBaseDeviceObject(owner->getDevice()),
	_owner(owner),
	_shaderModuleKey(shaderModuleKey),
	_repository(shaderModuleKey.codeSize != 0
		? owner->getDevice()->getShaderLibraryRepository()
		: nullptr) {}

MVKShaderLibrary* MVKShaderLibraryCache::getShaderLibrary(SPIRVToMSLConversionConfiguration* pShaderConfig,
														  MVKShaderModule* shaderModule, MVKPipeline* pipeline,
														  bool* pWasAdded, VkPipelineCreationFeedback* pShaderFeedback,
														  uint64_t startTime) {
	bool wasAdded = false;
	MVKShaderLibrary* shLib = findShaderLibrary(pShaderConfig, pShaderFeedback, startTime);
	if (!shLib && _repository) {
		shLib = _repository->acquire(_shaderModuleKey, pShaderConfig);
		if (shLib) {
			_shaderLibraries.emplace_back(*pShaderConfig, shLib);
			wasAdded = true;
			addPerformanceInterval(getPerformanceStats().shaderCompilation.shaderLibraryFromCache, startTime);
			if (pShaderFeedback) {
				pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime);
			}
		}
	}
	if ( !shLib && !pipeline->shouldFailOnPipelineCompileRequired() ) {
		SPIRVToMSLConversionResult conversionResult;
		if (shaderModule->convert(pShaderConfig, conversionResult)) {
			shLib = addShaderLibrary(pShaderConfig, conversionResult);
			if (pShaderFeedback) {
				pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime);
			}
			wasAdded = shLib != nullptr;
		}
	}

	if (pWasAdded) { *pWasAdded = wasAdded; }

	return shLib;
}

// Finds and returns a shader library matching the shader config, or returns nullptr if it doesn't exist.
// If a match is found, the shader config is aligned with the shader config of the matching library.
MVKShaderLibrary* MVKShaderLibraryCache::findShaderLibrary(SPIRVToMSLConversionConfiguration* pShaderConfig,
														   VkPipelineCreationFeedback* pShaderFeedback,
														   uint64_t startTime) {
	for (auto& slPair : _shaderLibraries) {
		if (slPair.first.matches(*pShaderConfig)) {
			pShaderConfig->alignWith(slPair.first);
			addPerformanceInterval(getPerformanceStats().shaderCompilation.shaderLibraryFromCache, startTime);
			if (pShaderFeedback) {
				pShaderFeedback->duration += mvkGetElapsedNanoseconds(startTime);
			}
			return slPair.second;
		}
	}
	return nullptr;
}

// Adds and returns a new shader library configured from the specified conversion configuration.
MVKShaderLibrary* MVKShaderLibraryCache::addShaderLibrary(const SPIRVToMSLConversionConfiguration* pShaderConfig,
														  const SPIRVToMSLConversionResult& conversionResult) {
	SPIRVToMSLConversionConfiguration alignedConfig = *pShaderConfig;
	if (_repository) {
		if (MVKShaderLibrary* existing = _repository->acquire(_shaderModuleKey, &alignedConfig)) {
			_shaderLibraries.emplace_back(alignedConfig, existing);
			return existing;
		}
	}

	VkResult priorConfigurationResult = _owner->getConfigurationResult();
	MVKShaderLibrary* candidate = new MVKShaderLibrary(_owner, conversionResult);
	bool candidateResident = candidate->isResident();
	MVKShaderLibrary* shLib = _repository
		? _repository->acquire(_shaderModuleKey, &alignedConfig, candidate)
		: candidate;
	if (_repository && !candidateResident && shLib &&
		priorConfigurationResult == VK_SUCCESS) {
		_owner->clearConfigurationResult();
	}
	if (shLib) { _shaderLibraries.emplace_back(alignedConfig, shLib); }
	return shLib;
}

// Adds and returns a new shader library configured from contents read from a pipeline cache.
MVKShaderLibrary* MVKShaderLibraryCache::addShaderLibrary(const SPIRVToMSLConversionConfiguration* pShaderConfig,
														  const SPIRVToMSLConversionResultInfo& resultInfo,
														  const MVKCompressor<std::string> compressedMSL) {
	SPIRVToMSLConversionConfiguration alignedConfig = *pShaderConfig;
	if (_repository) {
		if (MVKShaderLibrary* existing = _repository->acquire(_shaderModuleKey, &alignedConfig)) {
			_shaderLibraries.emplace_back(alignedConfig, existing);
			return existing;
		}
	}

	VkResult priorConfigurationResult = _owner->getConfigurationResult();
	MVKShaderLibrary* candidate = new MVKShaderLibrary(_owner, resultInfo, compressedMSL);
	bool candidateResident = candidate->isResident();
	MVKShaderLibrary* shLib = _repository
		? _repository->acquire(_shaderModuleKey, &alignedConfig, candidate)
		: candidate;
	if (_repository && !candidateResident && shLib &&
		priorConfigurationResult == VK_SUCCESS) {
		_owner->clearConfigurationResult();
	}
	if (shLib) { _shaderLibraries.emplace_back(alignedConfig, shLib); }
	return shLib;
}

// Merge another shader library cache with this one. Handle null input.
void MVKShaderLibraryCache::merge(MVKShaderLibraryCache* other) {
	if ( !other ) { return; }
	for (auto& otherPair : other->_shaderLibraries) {
		if ( !findShaderLibrary(&otherPair.first) ) {
			if (_repository) {
				SPIRVToMSLConversionConfiguration alignedConfig = otherPair.first;
				MVKShaderLibrary* shared = _repository->acquire(_shaderModuleKey, &alignedConfig);
				if (!shared) {
					VkResult priorConfigurationResult = _owner->getConfigurationResult();
					MVKShaderLibrary* candidate = new MVKShaderLibrary(
						_owner,
						otherPair.second->_shaderConversionResultInfo,
						otherPair.second->_compressedMSL);
					bool candidateResident = candidate->isResident();
					shared = _repository->acquire(_shaderModuleKey, &alignedConfig, candidate);
					if (!candidateResident && shared &&
						priorConfigurationResult == VK_SUCCESS) {
						_owner->clearConfigurationResult();
					}
				}
				if (shared) { _shaderLibraries.emplace_back(alignedConfig, shared); }
			} else {
				_shaderLibraries.emplace_back(otherPair.first, new MVKShaderLibrary(*otherPair.second));
				_shaderLibraries.back().second->_owner = _owner;
			}
		}
	}
}

MVKShaderLibraryCache::~MVKShaderLibraryCache() {
	for (auto& slPair : _shaderLibraries) {
		if (_repository) {
			_repository->release(_shaderModuleKey, slPair.first, slPair.second);
		} else {
			slPair.second->release();
		}
	}
}


#pragma mark -
#pragma mark MVKShaderModule

MVKMTLFunction MVKShaderModule::getMTLFunction(SPIRVToMSLConversionConfiguration* pShaderConfig,
											   const VkSpecializationInfo* pSpecializationInfo,
											   MVKPipeline* pipeline,
											   VkPipelineCreationFeedback* pShaderFeedback) {
	MVKShaderLibrary* mvkLib = _directMSLLibrary;
	if ( !mvkLib ) {
		uint64_t startTime = pShaderFeedback ? mvkGetTimestamp() : getPerformanceTimestamp();
		MVKPipelineCache* pipelineCache = pipeline->getPipelineCache();
		if (pipelineCache) {
			mvkLib = pipelineCache->getShaderLibrary(pShaderConfig, this, pipeline, pShaderFeedback, startTime);
		} else {
			lock_guard<mutex> lock(_accessLock);
			mvkLib = _shaderLibraryCache.getShaderLibrary(pShaderConfig, this, pipeline, nullptr, pShaderFeedback, startTime);
		}
	} else {
		mvkLib->setEntryPointName(pShaderConfig->options.entryPointName);
		pShaderConfig->markAllInterfaceVarsAndResourcesUsed();
	}

	return mvkLib
		? mvkLib->getMTLFunction(
			pSpecializationInfo,
			pShaderFeedback,
			this,
			!pipeline->shouldFailOnPipelineCompileRequired())
		: MVKMTLFunctionNull;
}

bool MVKShaderModule::convert(SPIRVToMSLConversionConfiguration* pShaderConfig,
							  SPIRVToMSLConversionResult& conversionResult) {
	const auto& mvkCfg = getMVKConfig();
	bool shouldLogCode = mvkCfg.debugMode;
	bool shouldLogEstimatedGLSL = shouldLogCode && mvkCfg.shaderLogEstimatedGLSL;

	uint64_t startTime = getPerformanceTimestamp();
	bool wasConverted = _spvConverter.convert(*pShaderConfig, conversionResult, shouldLogCode, shouldLogCode, shouldLogEstimatedGLSL);
	addPerformanceInterval(getPerformanceStats().shaderCompilation.spirvToMSL, startTime);

	const char* dumpDir = getMVKConfig().shaderDumpDir;
	if (dumpDir && *dumpDir) {
		char path[PATH_MAX];
		const char* type;
		switch (pShaderConfig->options.entryPointStage) {
			case spv::ExecutionModelVertex:                 type = "-vs"; break;
			case spv::ExecutionModelTessellationControl:    type = "-tcs"; break;
			case spv::ExecutionModelTessellationEvaluation: type = "-tes"; break;
			case spv::ExecutionModelFragment:               type = "-fs"; break;
			case spv::ExecutionModelGeometry:               type = "-gs"; break;
			case spv::ExecutionModelTaskNV:                 type = "-ts"; break;
			case spv::ExecutionModelMeshNV:                 type = "-ms"; break;
			case spv::ExecutionModelGLCompute:              type = "-cs"; break;
			default:                                        type = "";    break;
		}
		mkdir(dumpDir, 0755);
		snprintf(path, sizeof(path), "%s/shader%s-%016zx.spv", dumpDir, type, _key.codeHash);
		FILE* file = fopen(path, "wb");
		if (file) {
			fwrite(_spvConverter.getSPIRV().data(), sizeof(uint32_t), _spvConverter.getSPIRV().size(), file);
			fclose(file);
		}
		snprintf(path, sizeof(path), "%s/shader%s-%016zx.metal", dumpDir, type, _key.codeHash);
		file = fopen(path, "wb");
		if (file) {
			if (wasConverted) {
				fwrite(conversionResult.msl.data(), 1, conversionResult.msl.size(), file);
				fclose(file);
			} else {
				fputs("Failed to convert:\n", file);
				fwrite(conversionResult.resultLog.data(), 1, conversionResult.resultLog.size(), file);
				fclose(file);
			}
		}
	}

	if (wasConverted) {
		if (shouldLogCode) { MVKLogInfo("%s", conversionResult.resultLog.c_str()); }
	} else {
		reportError(VK_ERROR_INITIALIZATION_FAILED, "Unable to convert SPIR-V to MSL:\n%s", conversionResult.resultLog.c_str());
	}
	return wasConverted;
}

void MVKShaderModule::setWorkgroupSize(uint32_t x, uint32_t y, uint32_t z) {
	if(_directMSLLibrary) { _directMSLLibrary->setWorkgroupSize(x, y, z); }
}


#pragma mark Construction

MVKShaderModule::MVKShaderModule(MVKDevice* device,
								 const VkShaderModuleCreateInfo* pCreateInfo) : MVKVulkanAPIDeviceObject(device), _shaderLibraryCache(this) {

	_directMSLLibrary = nullptr;

	size_t codeSize = pCreateInfo->codeSize;

    // Ensure something is there.
    if ( (pCreateInfo->pCode == VK_NULL_HANDLE) || (codeSize < 4) ) {
		setConfigurationResult(reportError(VK_ERROR_INITIALIZATION_FAILED, "vkCreateShaderModule(): Shader module contains no shader code."));
		return;
	}

	size_t codeHash = 0;

	// Retrieve the magic number to determine what type of shader code has been loaded.
	// NOTE: Shader code should be submitted as SPIR-V. Although some simple direct MSL shaders may work,
	// direct loading of MSL source code or compiled MSL code is not officially supported at this time.
	// Future versions of MoltenVK may support direct MSL submission again.
	uint32_t magicNum = *pCreateInfo->pCode;
	switch (magicNum) {
		case kMVKMagicNumberSPIRVCode: {					// SPIR-V code
			size_t spvCount = (codeSize + 3) >> 2;			// Round up if byte length not exactly on uint32_t boundary

			uint64_t startTime = getPerformanceTimestamp();
			codeHash = mvkHash(pCreateInfo->pCode, spvCount);
			addPerformanceInterval(getPerformanceStats().shaderCompilation.hashShaderCode, startTime);

			_spvConverter.setSPIRV(pCreateInfo->pCode, spvCount);

			break;
		}
		case kMVKMagicNumberMSLSourceCode: {				// MSL source code
			size_t hdrSize = sizeof(MVKMSLSPIRVHeader);
			char* pMSLCode = (char*)(uintptr_t(pCreateInfo->pCode) + hdrSize);
			size_t mslCodeLen = codeSize - hdrSize;

			uint64_t startTime = getPerformanceTimestamp();
			codeHash = mvkHash(&magicNum);
			codeHash = mvkHash(pMSLCode, mslCodeLen, codeHash);
			addPerformanceInterval(getPerformanceStats().shaderCompilation.hashShaderCode, startTime);

			SPIRVToMSLConversionResult conversionResult;
			conversionResult.msl = pMSLCode;
			_directMSLLibrary = new MVKShaderLibrary(this, conversionResult);

			break;
		}
		case kMVKMagicNumberMSLCompiledCode: {				// MSL compiled binary code
			size_t hdrSize = sizeof(MVKMSLSPIRVHeader);
			char* pMSLCode = (char*)(uintptr_t(pCreateInfo->pCode) + hdrSize);
			size_t mslCodeLen = codeSize - hdrSize;

			uint64_t startTime = getPerformanceTimestamp();
			codeHash = mvkHash(&magicNum);
			codeHash = mvkHash(pMSLCode, mslCodeLen, codeHash);
			addPerformanceInterval(getPerformanceStats().shaderCompilation.hashShaderCode, startTime);

			_directMSLLibrary = new MVKShaderLibrary(this, (void*)(pMSLCode), mslCodeLen);

			break;
		}
		default:
			setConfigurationResult(reportError(VK_ERROR_INITIALIZATION_FAILED, "vkCreateShaderModule(): The SPIR-V contains an invalid magic number %x.", magicNum));
			break;
	}

	_key = MVKShaderModuleKey(codeSize, codeHash);
}

MVKShaderModule::~MVKShaderModule() {
	if (_directMSLLibrary) { _directMSLLibrary->destroy(); }
}


#pragma mark -
#pragma mark MVKShaderLibraryCompiler

id<MTLLibrary> MVKShaderLibraryCompiler::newMTLLibrary(NSString* mslSourceCode,
												   const SPIRVToMSLConversionResultInfo& shaderConversionResults,
												   const vector<pair<MSLSpecializationMacroInfo, MVKShaderMacroValue>>& specializationMacroDef) {
	auto mtlCompileOptions = [getDevice()->getMTLCompileOptions(
		shaderConversionResults.entryPoint.fpFastMathFlags,
		shaderConversionResults.isPositionInvariant) retain];
	if (!specializationMacroDef.empty()) {
		size_t macro_count = specializationMacroDef.size();
		NSString *macro_names[macro_count];
		NSNumber *macro_values[macro_count];
		for (uint32_t i = 0; i < specializationMacroDef.size(); i++) {
			macro_names[i] = @(specializationMacroDef[i].first.name.c_str());
			macro_values[i] = getMacroValue(
				specializationMacroDef[i].first,
				specializationMacroDef[i].second);
		}
		mtlCompileOptions.preprocessorMacros = [NSDictionary dictionaryWithObjects:macro_values
																	  forKeys:macro_names
																		count:macro_count];
	}
	logCompilation(mtlCompileOptions);

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	MVKMetal4CompilerService* metal4Compiler = getDevice()->getMetal4CompilerService();
	bool attemptedMetal4 = false;
	NSError* metal4Error = nil;
	if (metal4Compiler) {
		id<MTLLibrary> mtl4Library = metal4Compiler->newMTLLibrary(
			mslSourceCode,
			mtlCompileOptions,
			&metal4Error,
			&attemptedMetal4);
		if (mtl4Library) {
			compileComplete(mtl4Library, nil);
			[mtl4Library release];
			[mtlCompileOptions release];
			return [_mtlLibrary retain];
		}
		if (attemptedMetal4) {
			getDevice()->reportMessage(
				MVK_CONFIG_LOG_LEVEL_INFO,
				"Metal 4 library compile failed; using the legacy compiler once: %s",
				metal4Error.localizedDescription.UTF8String ?: "unknown error");
		}
	}
	uint64_t legacyCompileStart = metal4Compiler ? mvkGetTimestamp() : 0;
#endif

	unique_lock<mutex> lock(_completionLock);
	compile(lock, ^{
		@autoreleasepool {
			auto mtlDev = getMTLDevice();
			@synchronized (mtlDev) {
				[mtlDev newLibraryWithSource:mslSourceCode
									options:mtlCompileOptions
							completionHandler:^(id<MTLLibrary> mtlLib, NSError* error) {
								bool isLate = compileComplete(mtlLib, error);
								if (isLate) { destroy(); }
							}];
			}
		}
	});
	[mtlCompileOptions release];

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	if (metal4Compiler) {
		metal4Compiler->recordLegacyLibraryCompile(
			mvkGetElapsedNanoseconds(legacyCompileStart),
			_mtlLibrary != nil,
			attemptedMetal4);
	}
#endif

	return [_mtlLibrary retain];
}

NSNumber *MVKShaderLibraryCompiler::getMacroValue(const MSLSpecializationMacroInfo& info,
												  const MVKShaderMacroValue& value) {
	NSNumber *result;

	if (info.isFloat) {
		if (value.size == sizeof(double)) {
			result = [NSNumber numberWithDouble: value.value.f64];
		} else {
			result = [NSNumber numberWithFloat: value.value.f32];
		}
	} else {
		if (info.isSigned) {
			switch (value.size) {
				case 1:
					result = [NSNumber numberWithChar: value.value.si8];
					break;
				case 2:
					result = [NSNumber numberWithShort: value.value.si16];
					break;
				case 4:
					result = [NSNumber numberWithInt: value.value.si32];
					break;
				case 8:
					result = [NSNumber numberWithLongLong: value.value.si64];
					break;
				default:
					result = [NSNumber numberWithInt: value.value.si32];
					break;
			}
		} else {
			switch (value.size) {
				case 1:
					result = [NSNumber numberWithUnsignedChar: value.value.ui8];
					break;
				case 2:
					result = [NSNumber numberWithUnsignedShort: value.value.ui16];
					break;
				case 4:
					result = [NSNumber numberWithUnsignedInt: value.value.ui32];
					break;
				case 8:
					result = [NSNumber numberWithUnsignedLongLong: value.value.ui64];
					break;
				default:
					result = [NSNumber numberWithUnsignedInt: value.value.ui32];
					break;
			}
		}
	}

	return result;
}

void MVKShaderLibraryCompiler::handleError() {
	if (_mtlLibrary) {
		MVKLogInfo("%s compilation succeeded with warnings (Error code %li):\n%s", _compilerType.c_str(),
				   (long)_compileError.code, _compileError.localizedDescription.UTF8String);
	} else {
		MVKMetalCompiler::handleError();
	}
}

bool MVKShaderLibraryCompiler::compileComplete(id<MTLLibrary> mtlLibrary, NSError* compileError) {
	lock_guard<mutex> lock(_completionLock);

	_mtlLibrary = [mtlLibrary retain];		// retained
	return endCompile(compileError);
}

void MVKShaderLibraryCompiler::logCompilation(MTLCompileOptions* mtlCompOpt) {
	if ( !getMVKConfig().debugMode ) { return; }

#if MVK_XCODE_16
	if ([mtlCompOpt respondsToSelector: @selector(mathMode)]) {
		const char* mathModeName = "Unknown";
		switch (mtlCompOpt.mathMode) {
			case MTLMathModeFast:
				mathModeName = "Fast";
				break;
			case MTLMathModeRelaxed:
				mathModeName = "Relaxed";
				break;
			case MTLMathModeSafe:
				mathModeName = "Safe";
				break;
			default:
				break;
		}
		const char* mathFPFName = "Unknown";
		switch (mtlCompOpt.mathFloatingPointFunctions) {
			case MTLMathFloatingPointFunctionsFast:
				mathFPFName = "Fast";
				break;
			case MTLMathFloatingPointFunctionsPrecise:
				mathFPFName = "Precise";
				break;
			default:
				break;
		}
		MVKLogInfo("Compiling Metal shader with MathMode %s, MathFloatingPointFunctions %s, and PreserveInvariance %sabled.",
				   mathModeName, mathFPFName, mtlCompOpt.preserveInvariance ? "en" : "dis");
	} else
#endif
	{
		MVKLogInfo("Compiling Metal shader with FastMath %sabled and PreserveInvariance %sabled.",
				   mtlCompOpt.fastMathEnabled ? "en" : "dis", mtlCompOpt.preserveInvariance ? "en" : "dis");
	}
}


#pragma mark Construction

MVKShaderLibraryCompiler::~MVKShaderLibraryCompiler() {
	[_mtlLibrary release];
}


#pragma mark -
#pragma mark MVKFunctionSpecializer

id<MTLFunction> MVKFunctionSpecializer::newMTLFunction(id<MTLLibrary> mtlLibrary,
													   NSString* funcName,
													   MTLFunctionConstantValues* constantValues) {
	unique_lock<mutex> lock(_completionLock);

	compile(lock, ^{
		[mtlLibrary newFunctionWithName: funcName
						 constantValues: constantValues
					  completionHandler: ^(id<MTLFunction> mtlFunc, NSError* error) {
						  bool isLate = compileComplete(mtlFunc, error);
						  if (isLate) { destroy(); }
					  }];
	});

	return [_mtlFunction retain];
}

bool MVKFunctionSpecializer::compileComplete(id<MTLFunction> mtlFunction, NSError* compileError) {
	lock_guard<mutex> lock(_completionLock);

	_mtlFunction = [mtlFunction retain];		// retained
	return endCompile(compileError);
}

#pragma mark Construction

MVKFunctionSpecializer::~MVKFunctionSpecializer() {
	[_mtlFunction release];
}

// End of MVK shader compilation implementations.
