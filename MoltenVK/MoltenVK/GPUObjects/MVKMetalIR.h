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
struct MVKPipelineStageResourceInfo;

bool mvkMetalIREnabled();
bool mvkMetalIRCompilerAvailable();
void mvkMetalIRDestroyDevice(MVKDevice* device);
uint32_t mvkMetalIRDescriptorCount(const MVKDescriptorSetLayout* layout);
uint32_t mvkMetalIRDenseBinding(const MVKDescriptorSetLayout* layout, uint32_t binding);
uint32_t mvkMetalIRShadowBytes(const MVKDescriptorSetLayout* layout);
void mvkMetalIRUpdateDescriptor(const MVKDescriptorSetLayout* layout,
                                const MVKDescriptorBinding* binding,
                                const MVKDescriptorSet* set,
                                uint32_t first, uint32_t count);
void mvkPopulateMetalIRResidencyOperations(MVKPipelineLayout* layout,
                                          MVKPipelineStageResourceInfo& resources);

struct MVKMetalIRArtifact {
    id<MTLLibrary> library = nil;
    id<MTLFunction> function = nil;
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
    uint32_t descriptorCounts[8] = {};
    uint32_t pushConstantSize = 0;
    // The layout size defines the root-table ABI; usage belongs to this entry point.
    bool usesPushConstants = true;
    bool usesPointCoordinates = false;
    uint32_t runtimeFlags = 0;
    ~MVKMetalIRArtifact();
};

std::shared_ptr<MVKMetalIRArtifact> mvkCompileMetalIR(
    MVKPipeline* owner, MVKPipelineLayout* layout, MVKShaderModule* module,
    const VkPipelineShaderStageCreateInfo* stage, uint32_t vertexTransformFlags = 0, uint32_t runtimeOptions = 0);
