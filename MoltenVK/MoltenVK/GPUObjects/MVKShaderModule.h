/*
 * MVKShaderModule.h
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

#pragma once

#include "MVKDevice.h"
#include "MVKSync.h"
#include "MVKCodec.h"
#include "MVKSmallVector.h"
#include <MoltenVKShaderConverter/SPIRVToMSLConverter.h>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#import <Metal/Metal.h>

class MVKPipelineCache;
class MVKShaderCacheIterator;
class MVKShaderLibraryCache;
class MVKShaderLibraryRepository;
class MVKShaderModule;

#pragma mark -
#pragma mark MVKShaderModuleKey

typedef struct MVKShaderModuleKey {
	std::size_t codeSize;
	std::size_t codeHash;

	bool operator==(const MVKShaderModuleKey& rhs) const {
		return ((codeSize == rhs.codeSize) && (codeHash == rhs.codeHash));
	}
	MVKShaderModuleKey(std::size_t codeSize, std::size_t codeHash) : codeSize(codeSize), codeHash(codeHash) {}
	MVKShaderModuleKey() : MVKShaderModuleKey(0, 0) {}
} MVKShaderModuleKey;

namespace std {
	template <>
	struct hash<MVKShaderModuleKey> {
		std::size_t operator()(const MVKShaderModuleKey& k) const { return k.codeHash; }
	};
}

#pragma mark -
#pragma mark MVKShaderLibrary

/** A MTLFunction and corresponding result information resulting from a shader conversion. */
typedef struct MVKMTLFunction {
  mvk::SPIRVToMSLConversionResultInfo shaderConversionResults;
	MTLSize threadGroupSize;
	id<MTLFunction> getMTLFunction() { return _mtlFunction; }

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	MTL4FunctionDescriptor* getMTL4FunctionDescriptor() const { return _mtl4FunctionDescriptor; }
	const std::string& getMTL4FunctionKey() const { return _mtl4FunctionKey; }
	const std::string& getMTL4PointerFunctionKey() const { return _mtl4PointerFunctionKey; }
#endif

	MVKMTLFunction(id<MTLFunction> mtlFunc,
				   const mvk::SPIRVToMSLConversionResultInfo scRslts,
				   MTLSize tgSize
#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
				   , MTL4FunctionDescriptor* mtl4FuncDesc = nil,
				   std::string mtl4FuncKey = {},
				   std::string mtl4PointerFuncKey = {}
#endif
				   );
	MVKMTLFunction(const MVKMTLFunction& other);
	MVKMTLFunction& operator=(const MVKMTLFunction& other);
	MVKMTLFunction() {}
	~MVKMTLFunction();

private:
	id<MTLFunction> _mtlFunction = nil;

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	MTL4FunctionDescriptor* _mtl4FunctionDescriptor = nil;
	std::string _mtl4FunctionKey;
	std::string _mtl4PointerFunctionKey;
#endif

} MVKMTLFunction;

/** A MVKMTLFunction indicating an invalid MTLFunction. The mtlFunction member is nil. */
const MVKMTLFunction MVKMTLFunctionNull(nil, mvk::SPIRVToMSLConversionResultInfo(), MTLSizeMake(1, 1, 1));

typedef struct MVKShaderMacroValue {
	union {
		int8_t si8;
		uint8_t ui8;
		int16_t si16;
		uint16_t ui16;
		int32_t si32;
		uint32_t ui32;
		int64_t si64;
		uint64_t ui64;
		float f32;
		double f64;
	} value;
	size_t size;

	inline bool operator<(const MVKShaderMacroValue& other) const {
		return value.ui64 < other.value.ui64 ||
			   (value.ui64 == other.value.ui64 && size < other.size);
	}
} MVKShaderMacroValue;

/**
 * Wraps a single MTLLibrary or a set of MTLLibrary variants with macro-based specialization
 *
 * The latter case is used when Vulkan specialization constants cannot be realized with
 * Metal function constants. Those specialization constants are turned into macros, and
 * when specialized, we have to *recompile* the MTLLibrary from source.
 *
 * To keep the details transparent to users, when specialization on macro occurs,
 * MVKShaderLibrary creates specialized variants (each one also a MVKShaderLibrary) behind
 * the scene and cache them in a map according to the macro-value mapping.
 */


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

class MVKShaderLibrary : public MVKBaseDeviceObject {

public:

	/** Returns the Vulkan API opaque object controlling this object. */
	MVKVulkanAPIObject* getVulkanAPIObject() override { return _owner->getVulkanAPIObject(); };

	/** Retains one physical-library ownership reference. */
	void retain();

	/** Releases one physical-library ownership reference. */
	void release();

	/**
	 * Sets the entry point function name.
	 *
	 * This is usually set automatically during shader conversion from SPIR-V to MSL.
	 * For a library that was created directly from MSL, this function can be used to
	 * set the name of the function if it has a different name than the default main0().
	 */
	void setEntryPointName(std::string& funcName);

    /**
	 * Sets the number of threads in a single compute kernel workgroup, per dimension.
	 *
	 * This is usually set automatically during shader conversion from SPIR-V to MSL.
	 * For a library that was created directly from MSL, this function can be used to
	 * set the workgroup size..
	 */
    void setWorkgroupSize(uint32_t x, uint32_t y, uint32_t z);
    
	MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,
					 const mvk::SPIRVToMSLConversionResult& conversionResult);

	/**
	 * When specializationMacroDef is not null, creates a macro-specialized library
	 * specializationMacroDef contains (specialization id, value) mappings, should be sorted
	 */
	MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,
					 const mvk::SPIRVToMSLConversionResultInfo& resultInfo,
					 const MVKCompressor<std::string> compressedMSL,
					 const std::vector<std::pair<uint32_t, MVKShaderMacroValue>>* specializationMacroDef = nullptr);

	MVKShaderLibrary(MVKVulkanAPIDeviceObject* owner,
					 const void* mslCompiledCodeData,
					 size_t mslCompiledCodeLength);

	MVKShaderLibrary(const MVKShaderLibrary& other);

	MVKShaderLibrary& operator=(const MVKShaderLibrary& other);

	~MVKShaderLibrary() override;

protected:
	friend MVKShaderCacheIterator;
	friend MVKShaderLibraryCache;
	friend MVKShaderLibraryRepository;
	friend MVKShaderModule;

	MVKMTLFunction getMTLFunction(const VkSpecializationInfo* pSpecializationInfo,
								  VkPipelineCreationFeedback* pShaderFeedback,
								  MVKShaderModule* shaderModule,
								  bool allowLibraryCompile);

	/** Returns whether the expensive Metal library payload is currently resident. */
	bool isResident() const { return _resident.load(std::memory_order_acquire); }

	/** Captures known physical payload bytes without waiting on an active library. */
	bool tryGetMemorySnapshot(MVKShaderLibraryMemorySnapshot& snapshot);

	/** Returns the repository-wide approximate LRU sequence of the last real use. */
	uint64_t getLastUseSequence() const { return _lastUseSequence.load(std::memory_order_relaxed); }

	/** Releases the resident payload only if no real use occurred after the LRU snapshot. */
	bool tryEvictResident(uint64_t expectedLastUseSequence);

	/** Moves a freshly compiled/imported payload into this cold canonical entry. */
	bool tryAdoptResidentPayload(MVKShaderLibrary* candidate);
	void handleCompilationError(NSError* err, const char* opDesc);
    MTLFunctionConstant* getFunctionConstant(NSArray<MTLFunctionConstant*>* mtlFCs, NSUInteger mtlFCID);
	void compileLibrary(const std::string& msl,
						const std::vector<std::pair<uint32_t, MVKShaderMacroValue> >* specializationMacroDef = nullptr);
	void compressMSL(const std::string& msl);
	void decompressMSL(std::string& msl);
	bool ensureResidentLocked(bool allowLibraryCompile);
	void touch();
	MVKCompressor<std::string>& getCompressedMSL() { return _compressedMSL; }

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	const std::string& getMetal4LibraryContentKey() const { return _metal4LibraryContentKey; }
#endif

	MVKVulkanAPIDeviceObject* _owner;
	id<MTLLibrary> _mtlLibrary = nil;
	MVKShaderLibraryRepository* _repository = nullptr;
	std::atomic<uint32_t> _referenceCount { 1 };
	std::atomic<uint64_t> _lastUseSequence { 0 };
	std::atomic<uint32_t> _activeUseCount { 0 };
	std::atomic<bool> _resident { false };
	std::atomic<bool> _repositoryResidentCounted { false };
	std::atomic<bool> _repositoryTracked { false };
	std::mutex _accessLock;
	MVKCompressor<std::string> _compressedMSL;
	mvk::SPIRVToMSLConversionResultInfo _shaderConversionResultInfo;

#if MVK_XCODE_26 && !MVK_TVOS && !MVK_VISIONOS && !MVK_OS_SIMULATOR
	std::string _metal4LibraryContentKey;
#endif

	/** When true, representing a library created with source, but never specialized */
	bool _maySpecializeWithMacro;
	/** Can only be populated when _maySpecializeWithMacro is true */
	std::map<std::vector<std::pair<uint32_t, MVKShaderMacroValue>>, MVKShaderLibrary *> _specializationVariants;
};


#pragma mark -
#pragma mark MVKShaderLibraryCache

/** Represents a cache of shader libraries for one shader module. */
class MVKShaderLibraryCache : public MVKBaseDeviceObject {

public:

	/** Returns the Vulkan API opaque object controlling this object. */
	MVKVulkanAPIObject* getVulkanAPIObject() override { return _owner->getVulkanAPIObject(); };

	/**
	 * Returns a shader library from the shader conversion configuration sourced from the
	 * shader module, lazily creating the shader library from source code in the shader
	 * module, if needed, and if the pipeline is not configured to fail if a pipeline compile
	 * is required. In that case, the new shader library is not created, and nil is returned.
	 *
	 * If pWasAdded is not nil, this function will set it to true if a new shader library was created,
	 * and to false if an existing shader library was found and returned.
	 */
	MVKShaderLibrary* getShaderLibrary(mvk::SPIRVToMSLConversionConfiguration* pShaderConfig,
									   MVKShaderModule* shaderModule, MVKPipeline* pipeline,
									   bool* pWasAdded, VkPipelineCreationFeedback* pShaderFeedback,
									   uint64_t startTime = 0);

	/** Adds this logical view's known bytes to a pipeline-cache snapshot. */
	void accumulateMemoryStatistics(MVKPipelineCacheMemoryStatistics* pStats) const;

	MVKShaderLibraryCache(MVKVulkanAPIDeviceObject* owner,
						  MVKShaderModuleKey shaderModuleKey = {});

	~MVKShaderLibraryCache() override;

protected:
	friend MVKShaderCacheIterator;
	friend MVKPipelineCache;
	friend MVKShaderModule;

	MVKShaderLibrary* findShaderLibrary(mvk::SPIRVToMSLConversionConfiguration* pShaderConfig,
										VkPipelineCreationFeedback* pShaderFeedback = nullptr,
										uint64_t startTime = 0);
	MVKShaderLibrary* addShaderLibrary(const mvk::SPIRVToMSLConversionConfiguration* pShaderConfig,
									   const mvk::SPIRVToMSLConversionResult& conversionResult);
	MVKShaderLibrary* addShaderLibrary(const mvk::SPIRVToMSLConversionConfiguration* pShaderConfig,
									   const mvk::SPIRVToMSLConversionResultInfo& resultInfo,
									   const MVKCompressor<std::string> compressedMSL);
	void merge(MVKShaderLibraryCache* other);

	MVKVulkanAPIDeviceObject* _owner;
	MVKShaderModuleKey _shaderModuleKey;
	MVKShaderLibraryRepository* _repository;
	MVKSmallVector<std::pair<mvk::SPIRVToMSLConversionConfiguration, MVKShaderLibrary*>> _shaderLibraries;
};


#pragma mark -
#pragma mark MVKShaderLibraryRepository

/**
 * Device-owned physical shader-library repository. VkPipelineCache objects keep
 * independent logical membership lists, while matching shader/config pairs point
 * at one canonical MVKShaderLibrary allocation.
 */
class MVKShaderLibraryRepository : public MVKVulkanAPIDeviceObject {

public:
	VkObjectType getVkObjectType() override { return VK_OBJECT_TYPE_UNKNOWN; }
	VkDebugReportObjectTypeEXT getVkDebugReportObjectType() override { return VK_DEBUG_REPORT_OBJECT_TYPE_UNKNOWN_EXT; }

	MVKShaderLibrary* acquire(MVKShaderModuleKey shaderModuleKey,
							  mvk::SPIRVToMSLConversionConfiguration* pShaderConfig,
							  MVKShaderLibrary* candidate = nullptr);

	void release(MVKShaderModuleKey shaderModuleKey,
					 const mvk::SPIRVToMSLConversionConfiguration& shaderConfig,
					 MVKShaderLibrary* library);

	/** Returns a monotonically increasing approximate LRU sequence. */
	uint64_t nextUseSequence();

	/** Accounts for a cold entry becoming resident and applies the configured budget. */
	void libraryBecameResident(MVKShaderLibrary* library, bool rehydrated, uint64_t rehydrateNanoseconds = 0);

	/** Records an attempted cold-entry rehydrate that failed. */
	void recordRehydrateFailure(uint64_t rehydrateNanoseconds);

	/** Accounts for a resident entry becoming cold. */
	void libraryBecameCold(
		MVKShaderLibrary* library,
		uint64_t evictedUncompressedMSLBytes);

	/** Evicts coldest resident payloads without removing logical cache membership. */
	void trimToResidentLimit(MVKShaderLibrary* protectedLibrary = nullptr);

	/** Returns a nonblocking physical repository memory and reclaim snapshot. */
	void getMemoryStatistics(MVKMetal4ShaderLibraryRepositoryStatistics* pStats);

	size_t getResidentLimit() const { return _residentLimit; }
	size_t getResidentCount() const { return _residentEntryCount.load(std::memory_order_relaxed); }

	/** Creates the experimental repository only when its environment gate is present. */
	static MVKShaderLibraryRepository* create(MVKDevice* device);

	MVKShaderLibraryRepository(MVKDevice* device, size_t residentLimit);
	~MVKShaderLibraryRepository() override;

protected:
	void propagateDebugName() override {}

private:
	struct Entry {
		mvk::SPIRVToMSLConversionConfiguration shaderConfig;
		MVKShaderLibrary* library;
		uint32_t membershipCount;
	};

	void untrackResident(MVKShaderLibrary* library);

	std::mutex _lock;
	std::mutex _trimLock;
	std::atomic<uint64_t> _nextUseSequence { 0 };
	std::atomic<size_t> _residentEntryCount { 0 };
	std::atomic<size_t> _residentPeakCount { 0 };
	size_t _residentLimit = 0;
	size_t _residentTrimHighWater = 0;
	std::atomic<uint64_t> _canonicalPublishCount { 0 };
	std::atomic<uint64_t> _logicalMembershipCount { 0 };
	std::atomic<uint64_t> _logicalMembershipPeak { 0 };
	std::atomic<uint64_t> _dedupeHitCount { 0 };
	std::atomic<uint64_t> _raceLoserCount { 0 };
	std::atomic<uint64_t> _residentEvictionCount { 0 };
	std::atomic<uint64_t> _evictedUncompressedMSLBytes { 0 };
	std::atomic<uint64_t> _residentAdoptionCount { 0 };
	std::atomic<uint64_t> _rehydrateCount { 0 };
	std::atomic<uint64_t> _rehydrateFailureCount { 0 };
	std::atomic<uint64_t> _rehydrateTotalNanoseconds { 0 };
	std::atomic<uint64_t> _rehydrateMaximumNanoseconds { 0 };
	std::atomic<uint64_t> _trimCycleCount { 0 };
	std::atomic<uint64_t> _trimBusyCount { 0 };
	std::atomic<uint64_t> _trimCandidateCount { 0 };
	std::atomic<uint64_t> _trimTotalNanoseconds { 0 };
	std::atomic<uint64_t> _trimMaximumNanoseconds { 0 };
	std::unordered_map<MVKShaderModuleKey, std::vector<Entry>> _entries;
};


#pragma mark -
#pragma mark MVKShaderModule

/** Represents a Vulkan shader module. */
class MVKShaderModule : public MVKVulkanAPIDeviceObject {

public:

	/** Returns the Vulkan type of this object. */
	VkObjectType getVkObjectType() override { return VK_OBJECT_TYPE_SHADER_MODULE; }

	/** Returns the debug report object type of this object. */
	VkDebugReportObjectTypeEXT getVkDebugReportObjectType() override { return VK_DEBUG_REPORT_OBJECT_TYPE_SHADER_MODULE_EXT; }

	/** Returns the Metal shader function, possibly specialized. */
	MVKMTLFunction getMTLFunction(mvk::SPIRVToMSLConversionConfiguration* pShaderConfig,
								  const VkSpecializationInfo* pSpecializationInfo,
								  MVKPipeline* pipeline,
								  VkPipelineCreationFeedback* pShaderFeedback);

	/** Convert the SPIR-V to MSL, using the specified shader conversion configuration. */
	bool convert(mvk::SPIRVToMSLConversionConfiguration* pShaderConfig,
               mvk::SPIRVToMSLConversionResult& conversionResult);

	/** Returns the original SPIR-V code that was specified when this object was created. */
	const std::vector<uint32_t>& getSPIRV() { return _spvConverter.getSPIRV(); }

    /** Sets the number of threads in a single compute kernel workgroup, per dimension. */
    void setWorkgroupSize(uint32_t x, uint32_t y, uint32_t z);
    
	/** Returns a key as a means of identifying this shader module in a pipeline cache. */
	MVKShaderModuleKey getKey() { return _key; }

	MVKShaderModule(MVKDevice* device, const VkShaderModuleCreateInfo* pCreateInfo);

	~MVKShaderModule() override;

protected:
	friend MVKShaderCacheIterator;

	void propagateDebugName() override {}

	MVKShaderLibraryCache _shaderLibraryCache;
	mvk::SPIRVToMSLConverter _spvConverter;
	MVKShaderLibrary* _directMSLLibrary;
	MVKShaderModuleKey _key;
    std::mutex _accessLock;
};


#pragma mark -
#pragma mark MVKShaderLibraryCompiler

/**
 * Creates a MTLLibrary from source code.
 *
 * Instances of this class are one-shot, and can only be used for a single library compilation.
 */
class MVKShaderLibraryCompiler : public MVKMetalCompiler {

public:

	/**
	 * Returns a new (retained) MTLLibrary object compiled from the MSL source code.
	 *
	 * If the Metal library compiler does not return within MVKConfiguration::metalCompileTimeout
	 * nanoseconds, an error will be generated and logged, and nil will be returned.
	 */
	id<MTLLibrary> newMTLLibrary(NSString* mslSourceCode,
								 const mvk::SPIRVToMSLConversionResultInfo& shaderConversionResults,
								 const std::vector<std::pair<mvk::MSLSpecializationMacroInfo, MVKShaderMacroValue>>& macroDef);


#pragma mark Construction

	MVKShaderLibraryCompiler(MVKVulkanAPIDeviceObject* owner) : MVKMetalCompiler(owner) {
		_compilerType = "Shader library";
		_pPerformanceTracker = &getPerformanceStats().shaderCompilation.mslCompile;
	}

	~MVKShaderLibraryCompiler() override;

protected:
	NSNumber *getMacroValue(const mvk::MSLSpecializationMacroInfo& info, const MVKShaderMacroValue& value);
	bool compileComplete(id<MTLLibrary> mtlLibrary, NSError *error);
	void handleError() override;
	void logCompilation(MTLCompileOptions*);

	id<MTLLibrary> _mtlLibrary = nil;
};


#pragma mark -
#pragma mark MVKFunctionSpecializer

/**
 * Compiles a specialized MTLFunction.
 *
 * Instances of this class are one-shot, and can only be used for a single function compilation.
 */
class MVKFunctionSpecializer : public MVKMetalCompiler {

public:

	/**
	 * Returns a new (retained) MTLFunction object compiled from the MTLLibrary and specialization constants.
	 *
	 * If the Metal function compiler does not return within MVKConfiguration::metalCompileTimeout
	 * nanoseconds, an error will be generated and logged, and nil will be returned.
	 */
	id<MTLFunction> newMTLFunction(id<MTLLibrary> mtlLibrary, NSString* funcName, MTLFunctionConstantValues* constantValues);


#pragma mark Construction

	MVKFunctionSpecializer(MVKVulkanAPIDeviceObject* owner) : MVKMetalCompiler(owner) {
		_compilerType = "Function specialization";
		_pPerformanceTracker = &getPerformanceStats().shaderCompilation.functionSpecialization;
	}

	~MVKFunctionSpecializer() override;

protected:
	bool compileComplete(id<MTLFunction> mtlFunction, NSError *error);

	id<MTLFunction> _mtlFunction = nil;
};
