#include "MVKMetalIR.h"
#include "MVKReplayTrace.h"
#include "MVKPipeline.h"
#include "MVKImage.h"
#include "MVKPixelFormats.h"
#include "mvk_datatypes.h"
#include <algorithm>
#include <cstring>

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
uint32_t mvkMetalIRTableBytes(const MVKDescriptorSetLayout* layout) {
    return layout->metalIRTableBytes();
}
uint32_t mvkMetalIRDescriptorTableMask(VkDescriptorType type) {
    switch (type) {
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return 1;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER: return 6;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT: return 2;
        case VK_DESCRIPTOR_TYPE_SAMPLER: return 8;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return 10;
        default: return 0;
    }
}
namespace {
struct MetalIREntry { uint64_t address, texture, metadata; };
static_assert(sizeof(MetalIREntry) == 24);

template<VkDescriptorType Type>
void writeRange(const MVKDescriptorSetLayout* layout, const MVKDescriptorBinding& binding,
                const MVKDescriptorSet* set, const void* source, size_t stride,
                uint32_t first, uint32_t count, const MVKMetal4TextureViewBinding* views) {
    auto* entries = reinterpret_cast<MetalIREntry*>(set->gpuBuffer);
    const auto* src = static_cast<const char*>(source);
    const uint32_t cpuStride = descriptorCPUSize(binding.cpuLayout);
    for (uint32_t i = 0; i < count; ++i, src += stride) {
        const uint32_t element = first + i;
        const auto* offsets = binding.metalIRTableOffsets;
        const char* cpu = set->cpuBuffer ? set->cpuBuffer + binding.cpuOffset + element * cpuStride : nullptr;
        if constexpr (Type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || Type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            const auto& value = *reinterpret_cast<const MVKCPUDescriptorOneID2Meta*>(cpu);
            id<MTLBuffer> buffer = value.a;
            MetalIREntry entry{buffer ? buffer.gpuAddress + value.offset : 0, 0, value.meta.buffer.size};
            if constexpr (Type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) entries[offsets[0] + element] = entry;
            else entries[offsets[1] + element] = entries[offsets[2] + element] = entry;
        } else if constexpr (Type == VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER || Type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) {
            id<MTLTexture> texture = *reinterpret_cast<id<MTLTexture> const*>(cpu);
            id<MTLBuffer> buffer = texture.buffer;
            uint64_t size = texture.textureType == MTLTextureTypeTextureBuffer
                ? texture.width * mvkMTLPixelFormatBytesPerBlock(texture.pixelFormat)
                : texture.height * texture.bufferBytesPerRow;
            MetalIREntry entry{buffer ? buffer.gpuAddress + texture.bufferOffset : 0,
                texture ? texture.gpuResourceID._impl : 0, (size & UINT32_MAX) | (1ull << 63)};
            entries[offsets[1] + element] = entry;
            if constexpr (Type == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) entries[offsets[2] + element] = entry;
        } else {
            if constexpr (Type != VK_DESCRIPTOR_TYPE_SAMPLER) {
                // CPU storage can hold a residency texture rather than the view.
                // Preserve the resolved pooled view ID from the original write.
                id<MTLTexture> texture = cpu ? *reinterpret_cast<id<MTLTexture> const*>(cpu) : nil;
                uint64_t resource = views ? views[i].resourceID._impl : texture ? texture.gpuResourceID._impl : 0;
                entries[offsets[1] + element] = {0, resource, 0};
                if constexpr (Type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) entries[offsets[2] + element] = entries[offsets[1] + element];
            }
            if constexpr (Type == VK_DESCRIPTOR_TYPE_SAMPLER || Type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                if (!binding.hasImmutableSamplers()) {
                    const auto* info = reinterpret_cast<const VkDescriptorImageInfo*>(src);
                    auto* sampler = reinterpret_cast<MVKSampler*>(info->sampler);
                    entries[offsets[3] + element] = {sampler ? sampler->getMTLSamplerState().gpuResourceID._impl : 0, 0, 0};
                }
            }
        }
    }
}

bool seekBinding(const MVKDescriptorBinding*& binding, const MVKDescriptorBinding* end, uint32_t& first) {
    while (binding < end && first >= binding->descriptorCount) {
        first -= binding->descriptorCount;
        ++binding;
    }
    assert(binding < end);
    return binding < end;
}
}

void mvkInitializeMetalIRDescriptors(const MVKDescriptorSetLayout* layout, const MVKDescriptorSet* set) {
    auto* entries = reinterpret_cast<MetalIREntry*>(set->gpuBuffer);
    for (const auto& binding : layout->bindings()) {
        if (!binding.hasImmutableSamplers()) continue;
        auto samplers = layout->immutableSamplers().data() + binding.immSamplerIndex;
        for (uint32_t i = 0; i < binding.descriptorCount; ++i)
            entries[binding.metalIRTableOffsets[3] + i] = {samplers[i]->getMTLSamplerState().gpuResourceID._impl, 0, 0};
    }
}

void mvkWriteMetalIRDescriptors(const MVKDescriptorSetLayout* layout, const MVKDescriptorBinding* binding,
                               const MVKDescriptorSet* set, const void* src, size_t stride,
                               uint32_t first, uint32_t count, const MVKMetal4TextureViewBinding* views) {
    mvkreplay::DescriptorRangeTimer trace(mvkreplay::IRShadowUpdate);
    // The layout-selected outer writer splits cross-binding writes before both
    // CPU residency metadata and GPU entries are updated.
    assert(first <= binding->descriptorCount && count <= binding->descriptorCount - first);
    switch (binding->descriptorType) {
#define IR_WRITE(type) case type: writeRange<type>(layout, *binding, set, src, stride, first, count, views); break
        IR_WRITE(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        IR_WRITE(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        IR_WRITE(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
        IR_WRITE(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER);
        IR_WRITE(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        IR_WRITE(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        IR_WRITE(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT);
        IR_WRITE(VK_DESCRIPTOR_TYPE_SAMPLER);
        IR_WRITE(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
#undef IR_WRITE
        default: assert(false); break; // Unsupported layouts fail at creation.
    }
}

void mvkCopyMetalIRDescriptors(const MVKDescriptorSetLayout* dstLayout,
    const MVKDescriptorBinding* srcBinding, const MVKDescriptorSet* srcSet, id<MTLArgumentEncoder>,
    const MVKDescriptorBinding* dstBinding, const MVKDescriptorSet* dstSet, id<MTLArgumentEncoder>,
    uint32_t srcFirst, uint32_t dstFirst, uint32_t count) {
    mvkreplay::DescriptorRangeTimer trace(mvkreplay::IRShadowUpdate);
    const auto* srcLayout = srcSet->layout;
    assert(srcLayout->isMetalIRStorage() && dstLayout->isMetalIRStorage());
    const auto* srcEnd = srcLayout->bindings().end();
    const auto* dstEnd = dstLayout->bindings().end();
    const auto* srcEntries = reinterpret_cast<const MetalIREntry*>(srcSet->gpuBuffer);
    auto* dstEntries = reinterpret_cast<MetalIREntry*>(dstSet->gpuBuffer);
    while (count && seekBinding(srcBinding, srcEnd, srcFirst) && seekBinding(dstBinding, dstEnd, dstFirst)) {
        assert(srcBinding->descriptorType == dstBinding->descriptorType);
        const uint32_t length = std::min(count, std::min(srcBinding->descriptorCount - srcFirst, dstBinding->descriptorCount - dstFirst));
        const uint32_t srcStride = descriptorCPUSize(srcBinding->cpuLayout), dstStride = descriptorCPUSize(dstBinding->cpuLayout);
        if (dstStride) {
            char* dst = dstSet->cpuBuffer + dstBinding->cpuOffset + dstFirst * dstStride;
            const char* src = srcStride ? srcSet->cpuBuffer + srcBinding->cpuOffset + srcFirst * srcStride : nullptr;
            if (srcStride == dstStride) memmove(dst, src, length * dstStride);
            else for (uint32_t i = 0; i < length; ++i) {
                // Immutable samplers may remove a CPU sampler slot. GPU entries
                // below retain/copy the right sampler; CPU records serve residency.
                const uint32_t bytes = std::min(srcStride, dstStride);
                if (bytes) memcpy(dst + i * dstStride, src + i * srcStride, bytes);
                memset(dst + i * dstStride + bytes, 0, dstStride - bytes);
            }
        }
        const uint32_t mask = mvkMetalIRDescriptorTableMask(dstBinding->descriptorType) &
            (dstBinding->hasImmutableSamplers() ? ~8u : ~0u);
        for (uint32_t table = 0; table < 4; ++table) if (mask & (1u << table))
            memmove(dstEntries + dstBinding->metalIRTableOffsets[table] + dstFirst,
                    srcEntries + srcBinding->metalIRTableOffsets[table] + srcFirst,
                    length * sizeof(MetalIREntry));
        count -= length; srcFirst += length; dstFirst += length;
    }
}
