#include "MVKReplayTrace.h"
#include "MVKMetalIR.h"
#include "MVKPipeline.h"
#include "MVKPixelFormats.h"
#include "mvk_datatypes.h"

// The IR root table is bound by its own encoder. This script only declares
// residency for the Vulkan resources actually used by the compiled entry.
// It deliberately has no MSL register numbers or source-conversion config.
void mvkPopulateMetalIRResidencyOperations(MVKPipelineLayout* layout,
                                          MVKPipelineStageResourceInfo& resources) {
    assert(resources.metalIR && resources.bindScript.ops.empty());
    if (layout->getDevice()->hasResidencySet()) return;
    const auto& artifact = *resources.metalIR;
    for (uint32_t set = 0; set < artifact.setCount; ++set) {
        const auto* setLayout = layout->getDescriptorSetLayout(set);
        assert(setLayout->argBufMode() == MVKArgumentBufferMode::Metal3);
        auto descriptors = setLayout->bindings();
        for (uint32_t index = 0; index < descriptors.size(); ++index) {
            const auto& descriptor = descriptors[index];
            if (!descriptor.descriptorCount || !artifact.usesBinding(set, descriptor.binding)) continue;
            bool liveCheck = mvkIsAnyFlagEnabled(descriptor.flags, MVK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT) ||
                             layout->getMVKConfig().liveCheckAllResources;
            auto useTexture = liveCheck ? MVKDescriptorBindOperationCode::UseTextureWithLiveCheck : MVKDescriptorBindOperationCode::UseResource;
            auto useBuffer = liveCheck ? MVKDescriptorBindOperationCode::UseBufferWithLiveCheck : MVKDescriptorBindOperationCode::UseResource;
            uint32_t writeable = descriptorIsWriteable(descriptor.descriptorType);
            for (uint32_t texture = 0; texture < descriptorTextureCount(descriptor.gpuLayout); ++texture)
                resources.bindScript.ops.push_back({useTexture, set, writeable, index, sizeof(id) * texture});
            if (descriptorHasBuffer(descriptor.gpuLayout))
                resources.bindScript.ops.push_back({useBuffer, set, writeable, index, descriptor.perDescriptorResourceCount.texture * sizeof(id)});
        }
    }
}

uint32_t mvkMetalIRDescriptorCount(const MVKDescriptorSetLayout* layout) {
    return layout->metalIRDescriptorCount();
}
uint32_t mvkMetalIRDenseBinding(const MVKDescriptorSetLayout* layout,uint32_t binding) {
    uint32_t index=layout->getBindingIndex(binding);
    return index<layout->bindings().size()?layout->bindings()[index].metalIRDenseOffset:UINT32_MAX;
}
uint32_t mvkMetalIRShadowBytes(const MVKDescriptorSetLayout* layout) {
    return layout->metalIRShadowBytes();
}
struct MetalIREntry {uint64_t address,texture,metadata;};
static_assert(sizeof(MetalIREntry)==24);
static void updateMetalIRDescriptorRange(const MVKDescriptorSetLayout* layout,const MVKDescriptorBinding* binding,
                                const MVKDescriptorSet* set,uint32_t first,uint32_t count,uint32_t n,uint32_t dense) {
    mvkreplay::Timer trace(mvkreplay::IRShadowUpdate);
    if(dense==UINT32_MAX||first+count>binding->descriptorCount)return;
    auto* shadow=(MetalIREntry*)(set->gpuBuffer+layout->metalIRShadowBase());
    for(uint32_t i=first;i<first+count;++i) {
        uint32_t index=dense+i;
        // A fixed Vulkan descriptor type only exposes its matching table(s).
        // Every relevant entry is assigned in full below. Clearing unrelated
        // CBV/SRV/UAV/sampler entries on every update adds hot-path stores.
        const char* cpu=set->cpuBuffer?set->cpuBuffer+binding->cpuOffset+i*descriptorCPUSize(binding->cpuLayout):nullptr;
        id object=cpu&&descriptorCPUSize(binding->cpuLayout)>=sizeof(id)?*(id const*)cpu:nil;
        const auto* gpuIDs=reinterpret_cast<const uint64_t*>(set->gpuBuffer+binding->gpuOffset);
        switch(binding->descriptorType) {
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC: {
                auto* desc=(const MVKCPUDescriptorOneID2Meta*)cpu;
                uint32_t block=binding->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER||binding->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC?0:2;
                // The normal Metal3 descriptor update already resolved this
                // address, including offsets and null descriptors. Reuse it
                // instead of querying the same Metal buffer a second time.
                uint64_t address=gpuIDs[i];
                shadow[block*n+index]={address,0,desc->meta.buffer.size};
                // DXIL may expose a readonly SSBO as SRV, while stores use UAV.
                if(block==2)shadow[n+index]=shadow[2*n+index];
                break;
            }
            case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: {
                // Native texture-buffer descriptors may retain only one object.
                // Reading a two-object CPU record here reads the next descriptor.
                id<MTLTexture> texture=object;
                id<MTLBuffer> buffer=texture.buffer;
                uint64_t offset=texture.bufferOffset;
                uint64_t byteCount=texture.textureType==MTLTextureTypeTextureBuffer
                    ? texture.width*mvkMTLPixelFormatBytesPerBlock(texture.pixelFormat)
                    : texture.height*texture.bufferBytesPerRow;
                uint32_t block=binding->descriptorType==VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER?1:2;
                shadow[block*n+index]={buffer?buffer.gpuAddress+offset:0,gpuIDs[i],(byteCount&UINT32_MAX)|(1ull<<63)};
                if(block==2)shadow[n+index]=shadow[2*n+index];
                break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
            case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
                uint32_t block=binding->descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_IMAGE?2:1;
                // The normal encoder carries the view ID, including pooled views.
                // Its CPU object can be only the backing texture for residency.
                shadow[block*n+index]={0,gpuIDs[i],0};
                if(block==2)shadow[n+index]=shadow[2*n+index];
                if(binding->descriptorType!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)break;
                uint32_t planes=binding->gpuLayout==MVKDescriptorGPULayout::Tex3SampSoA?3:
                    binding->gpuLayout==MVKDescriptorGPULayout::Tex2SampSoA?2:1;
                shadow[3*n+index]={gpuIDs[planes*binding->descriptorCount+i],0,0};
                break;
            }
            case VK_DESCRIPTOR_TYPE_SAMPLER: {
                shadow[3*n+index]={gpuIDs[i],0,0};
                break;
            }
            default:
                // Preserve fail-closed contents for unsupported descriptor kinds.
                shadow[index]={};shadow[n+index]={};shadow[2*n+index]={};shadow[3*n+index]={};
                break;
        }
    }
}
void mvkMetalIRUpdateDescriptor(const MVKDescriptorSetLayout* layout,const MVKDescriptorBinding* binding,
                                const MVKDescriptorSet* set,uint32_t first,uint32_t count) {
    if(!mvkMetalIRShadowBytes(layout)||!set->gpuBuffer)return;
    uint32_t n=mvkMetalIRDescriptorCount(layout);
    // Ordinary writes/templates fit one binding. Its immutable metadata is
    // already available, so avoid finding the same binding by number again.
    if(first<=binding->descriptorCount&&count<=binding->descriptorCount-first) {
        updateMetalIRDescriptorRange(layout,binding,set,first,count,n,binding->metalIRDenseOffset);
        return;
    }
    size_t index=layout->getBindingIndex(binding->binding);
    const auto bindings=layout->bindings();
    uint32_t dense=binding->metalIRDenseOffset;
    // Vulkan writes, copies and update templates may continue across adjacent
    // bindings. Mirror every affected range after the normal update completes.
    while(count&&index<bindings.size()) {
        const auto& current=bindings[index++];
        if(first>=current.descriptorCount){first-=current.descriptorCount;dense+=current.descriptorCount;continue;}
        uint32_t updated=std::min(count,current.descriptorCount-first);
        updateMetalIRDescriptorRange(layout,&current,set,first,updated,n,dense);
        count-=updated;first=0;dense+=current.descriptorCount;
    }
}
