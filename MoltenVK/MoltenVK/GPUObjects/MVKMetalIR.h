#pragma once
#include "MVKMetalIRBridge.h"
#include "MVKDescriptorSet.h"
#include <memory>
#include <vector>
#include <string>
#import <Metal/Metal.h>

class MVKShaderModule;
class MVKPipelineLayout;
class MVKPipeline;
class MVKDevice;
struct MVKMetalIRLifetimeCounters;
struct MVKPipelineStageResourceInfo;
struct MVKMetal4TextureViewBinding;

bool mvkMetalIREnabled();
bool mvkMetalIRCompilerAvailable();
// Seven cumulative counters: compiles, disk hits, Mesa, MSC, raster adapter,
// library/function load, reflection. Capacity >=8 adds rejected shaders as
// field7; capacity >=9 adds PSO creation wall nanoseconds as field8. Seven-field
// callers retain their original return/count contract.
// Durations are nanoseconds; opt-in only.
uint32_t mvkMetalIRCompilerStatistics(MVKDevice* device, uint64_t* output, uint32_t capacity);
uint32_t mvkMetalIRSetTelemetryEnabled(uint32_t enabled);
uint32_t mvkMetalIRCacheStatistics(MVKDevice* device, uint64_t* output, uint32_t capacity);
uint32_t mvkMetalIRSetProbeDiagnostics(uint32_t flags);
class MVKMetalIRPSOTimer {
    MVKDevice* _device=nullptr;
    uint64_t _start=0;
public:
    explicit MVKMetalIRPSOTimer(MVKDevice* device);
    ~MVKMetalIRPSOTimer();
};
VkResult mvkMetalIRConfigureCache(MVKDevice* device, const char* directory, uint64_t maxBytes);
void mvkMetalIRDestroyDevice(MVKDevice* device);
uint64_t mvkMetalIRRelieveCompilerMemory();
uint32_t mvkMetalIRCompilerAdmissionStatistics(uint64_t* output,uint32_t capacity);
uint32_t mvkMetalIRDescriptorCount(const MVKDescriptorSetLayout* layout);
uint32_t mvkMetalIRDenseBinding(const MVKDescriptorSetLayout* layout, uint32_t binding);
uint32_t mvkMetalIRDescriptorTableMask(VkDescriptorType type);
uint32_t mvkMetalIRTableBytes(const MVKDescriptorSetLayout* layout);
void mvkInitializeMetalIRDescriptors(const MVKDescriptorSetLayout*, const MVKDescriptorSet*);
void mvkWriteMetalIRDescriptors(const MVKDescriptorSetLayout*, const MVKDescriptorBinding*,
    const MVKDescriptorSet*, const void*, size_t, uint32_t, uint32_t,
    const MVKMetal4TextureViewBinding*);
void mvkCopyMetalIRDescriptors(const MVKDescriptorSetLayout*, const MVKDescriptorBinding*,
    const MVKDescriptorSet*, id<MTLArgumentEncoder>, const MVKDescriptorBinding*,
    const MVKDescriptorSet*, id<MTLArgumentEncoder>, uint32_t, uint32_t, uint32_t);
void mvkPopulateMetalIRResidencyOperations(MVKPipelineLayout* layout,
                                          MVKPipelineStageResourceInfo& resources);

// Encoding metadata survives independently of library/function wrappers.
struct MVKMetalIRMetadata {
    uint32_t threadgroupSize[3] = {1,1,1};
    uint64_t usedSets = 0;
    std::vector<uint64_t> usedBindings;
    bool usesBinding(uint32_t set,uint32_t binding) const {
        uint64_t key=((uint64_t)set<<32)|binding;
        for(uint64_t used:usedBindings)if(used==key)return true;
        return false;
    }
    uint64_t vertexLocations = 0;
    uint8_t vertexAttributes[32];
    uint32_t setCount = 0;
    uint32_t pushConstantSize = 0;
    // The layout size defines the root-table ABI; usage belongs to this entry point.
    bool usesPushConstants = true;
    bool usesPointCoordinates = false;
    uint32_t runtimeFlags = 0;
    bool needsDrawData() const {
        return runtimeFlags & (MVK_METAL_IR_RUNTIME_DATA | MVK_METAL_IR_DRAW_PARAMETERS | MVK_METAL_IR_DRAW_BASES);
    }
};

// Only construction and the bounded recent-artifact cache own Metal objects.
struct MVKMetalIRArtifact : MVKMetalIRMetadata {
    id<MTLLibrary> library = nil;
    id<MTLFunction> function = nil;
    std::shared_ptr<const MVKMetalIRMetadata> metadata;
    // Statistics ownership is independent of device lifetime. A cache eviction
    // can release the final artifact after the device map reference is removed.
    std::shared_ptr<MVKMetalIRLifetimeCounters> lifetimeCounters;
    uint64_t trackedMetallibBytes = 0;
    bool trackedFunction = false;
    ~MVKMetalIRArtifact();
};

std::shared_ptr<MVKMetalIRArtifact> mvkCompileMetalIR(
    MVKPipeline* owner, MVKPipelineLayout* layout, MVKShaderModule* module,
    const VkPipelineShaderStageCreateInfo* stage, uint32_t vertexTransformFlags = 0, uint32_t runtimeOptions = 0);
