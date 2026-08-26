#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) {
        fail(std::string(operation) + " failed with VkResult " + std::to_string(result));
    }
}

template <typename T>
T makeVkStruct(VkStructureType sType) {
    T value{};
    value.sType = sType;
    return value;
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& extension) {
        return std::strcmp(extension.extensionName, name) == 0;
    });
}

struct Buffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    bool coherent = false;

    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    Buffer(Buffer&& other) noexcept { *this = std::move(other); }
    Buffer& operator=(Buffer&& other) noexcept {
        if (this == &other) { return *this; }
        destroy();
        device = other.device;
        buffer = other.buffer;
        memory = other.memory;
        size = other.size;
        coherent = other.coherent;
        other.device = VK_NULL_HANDLE;
        other.buffer = VK_NULL_HANDLE;
        other.memory = VK_NULL_HANDLE;
        return *this;
    }

    ~Buffer() { destroy(); }

    void destroy() {
        if (device && buffer) { vkDestroyBuffer(device, buffer, nullptr); }
        if (device && memory) { vkFreeMemory(device, memory, nullptr); }
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }
};

struct Image {
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;

    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    Image(Image&& other) noexcept { *this = std::move(other); }
    Image& operator=(Image&& other) noexcept {
        if (this == &other) { return *this; }
        destroy();
        device = other.device;
        image = other.image;
        view = other.view;
        memory = other.memory;
        format = other.format;
        width = other.width;
        height = other.height;
        other.device = VK_NULL_HANDLE;
        other.image = VK_NULL_HANDLE;
        other.view = VK_NULL_HANDLE;
        other.memory = VK_NULL_HANDLE;
        return *this;
    }
    ~Image() { destroy(); }

    void destroy() {
        if (device && view) { vkDestroyImageView(device, view, nullptr); }
        if (device && image) { vkDestroyImage(device, image, nullptr); }
        if (device && memory) { vkFreeMemory(device, memory, nullptr); }
        view = VK_NULL_HANDLE;
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }
};

uint32_t chooseMemoryType(const VkPhysicalDeviceMemoryProperties& properties,
                          uint32_t typeBits,
                          VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred,
                          bool* coherent) {
    uint32_t fallback = UINT32_MAX;
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index) {
        if ((typeBits & (1u << index)) == 0) { continue; }
        const auto flags = properties.memoryTypes[index].propertyFlags;
        if ((flags & required) != required) { continue; }
        if ((flags & preferred) == preferred) {
            if (coherent) { *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0; }
            return index;
        }
        if (fallback == UINT32_MAX) { fallback = index; }
    }
    if (fallback == UINT32_MAX) { fail("No compatible Vulkan memory type"); }
    if (coherent) {
        *coherent = (properties.memoryTypes[fallback].propertyFlags &
                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    }
    return fallback;
}

Buffer createBuffer(VkPhysicalDevice physicalDevice, VkDevice device, VkDeviceSize size) {
    Buffer result;
    result.device = device;
    result.size = size;

    VkBufferCreateInfo createInfo = makeVkStruct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    createInfo.size = size;
    createInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vkCreateBuffer(device, &createInfo, nullptr, &result.buffer), "vkCreateBuffer");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(device, result.buffer, &requirements);

    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    bool coherent = false;
    const uint32_t memoryType = chooseMemoryType(
        memoryProperties,
        requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        &coherent);

    VkMemoryAllocateInfo allocateInfo = makeVkStruct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    check(vkAllocateMemory(device, &allocateInfo, nullptr, &result.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device, result.buffer, result.memory, 0), "vkBindBufferMemory");
    result.coherent = coherent;
    return result;
}

Image createImage(VkPhysicalDevice physicalDevice,
                  VkDevice device,
                  uint32_t width,
                  uint32_t height,
                  VkFormat format = VK_FORMAT_R8G8B8A8_UNORM,
                  VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    Image result;
    result.device = device;
    result.format = format;
    result.width = width;
    result.height = height;

    VkImageCreateInfo createInfo = makeVkStruct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    createInfo.imageType = VK_IMAGE_TYPE_2D;
    createInfo.format = result.format;
    createInfo.extent = {width, height, 1};
    createInfo.mipLevels = 1;
    createInfo.arrayLayers = 1;
    createInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    createInfo.usage = usage;
    createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(device, &createInfo, nullptr, &result.image), "vkCreateImage");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, result.image, &requirements);
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    const uint32_t memoryType = chooseMemoryType(
        memoryProperties,
        requirements.memoryTypeBits,
        0,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        nullptr);
    VkMemoryAllocateInfo allocateInfo = makeVkStruct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    check(vkAllocateMemory(device, &allocateInfo, nullptr, &result.memory), "vkAllocateMemory(image)");
    check(vkBindImageMemory(device, result.image, result.memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo viewInfo = makeVkStruct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    viewInfo.image = result.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = result.format;
    viewInfo.subresourceRange.aspectMask = aspect;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    check(vkCreateImageView(device, &viewInfo, nullptr, &result.view), "vkCreateImageView");
    return result;
}

VkCommandBuffer beginCommandBuffer(VkDevice device, VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocateInfo = makeVkStruct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
    allocateInfo.commandPool = commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo beginInfo = makeVkStruct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");
    return commandBuffer;
}

void endCommandBuffer(VkCommandBuffer commandBuffer) {
    check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
}

VkFence createFence(VkDevice device) {
    VkFenceCreateInfo createInfo = makeVkStruct<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
    VkFence fence = VK_NULL_HANDLE;
    check(vkCreateFence(device, &createInfo, nullptr, &fence), "vkCreateFence");
    return fence;
}

void waitFence(VkDevice device, VkFence fence) {
    check(vkWaitForFences(device, 1, &fence, VK_TRUE, 10'000'000'000ULL), "vkWaitForFences");
}

void validateRepeatedByte(VkDevice device, Buffer& buffer, uint8_t expected) {
    void* mapped = nullptr;
    check(vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped), "vkMapMemory");
    if (!buffer.coherent) {
        VkMappedMemoryRange range = makeVkStruct<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        check(vkInvalidateMappedMemoryRanges(device, 1, &range), "vkInvalidateMappedMemoryRanges");
    }
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    for (VkDeviceSize index = 0; index < buffer.size; ++index) {
        if (bytes[index] != expected) {
            const uint8_t actual = bytes[index];
            vkUnmapMemory(device, buffer.memory);
            fail("Readback mismatch at byte " + std::to_string(index) +
                 ": expected " + std::to_string(expected) +
                 ", got " + std::to_string(actual));
        }
    }
    vkUnmapMemory(device, buffer.memory);
}

void writeBytes(VkDevice device, Buffer& buffer, const std::vector<uint8_t>& bytes) {
    if (bytes.size() > buffer.size) { fail("writeBytes payload exceeds buffer"); }
    void* mapped = nullptr;
    check(vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped), "vkMapMemory(write)");
    std::memcpy(mapped, bytes.data(), bytes.size());
    if (!buffer.coherent) {
        VkMappedMemoryRange range = makeVkStruct<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        check(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges");
    }
    vkUnmapMemory(device, buffer.memory);
}

void validateBytes(VkDevice device, Buffer& buffer, const std::vector<uint8_t>& expected) {
    void* mapped = nullptr;
    check(vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped), "vkMapMemory(validate)");
    if (!buffer.coherent) {
        VkMappedMemoryRange range = makeVkStruct<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        check(vkInvalidateMappedMemoryRanges(device, 1, &range), "vkInvalidateMappedMemoryRanges(bytes)");
    }
    if (std::memcmp(mapped, expected.data(), expected.size()) != 0) {
        vkUnmapMemory(device, buffer.memory);
        fail("Image readback bytes did not match upload bytes");
    }
    vkUnmapMemory(device, buffer.memory);
}

void validateSolidColor(VkDevice device,
                        Buffer& buffer,
                        std::array<uint8_t, 4> expected,
                        uint8_t tolerance = 1) {
    void* mapped = nullptr;
    check(vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped),
          "vkMapMemory(validate solid color)");
    if (!buffer.coherent) {
        VkMappedMemoryRange range = makeVkStruct<VkMappedMemoryRange>(VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        check(vkInvalidateMappedMemoryRanges(device, 1, &range),
              "vkInvalidateMappedMemoryRanges(solid color)");
    }
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    for (VkDeviceSize index = 0; index < buffer.size; index += 4) {
        for (uint32_t channel = 0; channel < 4; ++channel) {
            const int delta = static_cast<int>(bytes[index + channel]) - expected[channel];
            if (delta < -static_cast<int>(tolerance) ||
                delta > static_cast<int>(tolerance)) {
                const uint8_t actual = bytes[index + channel];
                vkUnmapMemory(device, buffer.memory);
                fail("Render readback mismatch at byte " + std::to_string(index + channel) +
                     ": expected near " + std::to_string(expected[channel]) +
                     ", got " + std::to_string(actual));
            }
        }
    }
    vkUnmapMemory(device, buffer.memory);
}

void validateLeftHalfColor(VkDevice device,
                           Buffer& buffer,
                           uint32_t width,
                           uint32_t height,
                           std::array<uint8_t, 4> color,
                           uint8_t tolerance = 1) {
    void* mapped = nullptr;
    check(vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped),
          "vkMapMemory(validate dynamic viewport/scissor)");
    if (!buffer.coherent) {
        VkMappedMemoryRange range = makeVkStruct<VkMappedMemoryRange>(
            VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
        range.memory = buffer.memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        check(vkInvalidateMappedMemoryRanges(device, 1, &range),
              "vkInvalidateMappedMemoryRanges(dynamic viewport/scissor)");
    }
    const auto* bytes = static_cast<const uint8_t*>(mapped);
    const std::array<uint8_t, 4> clear{{0, 0, 0, 255}};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto& expected = x < width / 2 ? color : clear;
            const size_t pixelOffset = (static_cast<size_t>(y) * width + x) * 4;
            for (uint32_t channel = 0; channel < 4; ++channel) {
                const int delta = static_cast<int>(bytes[pixelOffset + channel]) -
                                  expected[channel];
                if (delta < -static_cast<int>(tolerance) ||
                    delta > static_cast<int>(tolerance)) {
                    const uint8_t actual = bytes[pixelOffset + channel];
                    vkUnmapMemory(device, buffer.memory);
                    fail("Dynamic viewport/scissor mismatch at pixel (" +
                         std::to_string(x) + ", " + std::to_string(y) +
                         ") channel " + std::to_string(channel) +
                         ": expected near " + std::to_string(expected[channel]) +
                         ", got " + std::to_string(actual));
                }
            }
        }
    }
    vkUnmapMemory(device, buffer.memory);
}

void imageBarrier(VkCommandBuffer commandBuffer,
                  VkImage image,
                  VkImageLayout oldLayout,
                  VkImageLayout newLayout,
                  VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess,
                  VkPipelineStageFlags srcStage,
                  VkPipelineStageFlags dstStage,
                  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) {
    VkImageMemoryBarrier barrier = makeVkStruct<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0,
                         0, nullptr, 0, nullptr, 1, &barrier);
}

VkShaderModule createDescriptorlessComputeShader(VkDevice device) {
    // SPIR-V for: #version 450; layout(local_size_x=1) in; void main() {}
    static constexpr uint32_t code[] = {
        0x07230203, 0x00010000, 0x00000000, 0x00000006, 0x00000000,
        0x00020011, 0x00000001,
        0x0003000e, 0x00000000, 0x00000001,
        0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000,
        0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001,
        0x00020013, 0x00000001,
        0x00030021, 0x00000002, 0x00000001,
        0x00050036, 0x00000001, 0x00000004, 0x00000000, 0x00000002,
        0x000200f8, 0x00000005,
        0x000100fd,
        0x00010038,
    };
    VkShaderModuleCreateInfo createInfo = makeVkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    createInfo.codeSize = sizeof(code);
    createInfo.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &createInfo, nullptr, &module), "vkCreateShaderModule(compute)");
    return module;
}

static constexpr uint32_t kRenderSmokeVertexSpirv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x00000028, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000018, 0x0000001c, 0x00030003,
    0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00050005, 0x0000000c, 0x69736f70, 0x6e6f6974, 0x00000073, 0x00060005,
    0x00000016, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006,
    0x00000016, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006,
    0x00000016, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000,
    0x00070006, 0x00000016, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
    0x0065636e, 0x00070006, 0x00000016, 0x00000003, 0x435f6c67, 0x446c6c75,
    0x61747369, 0x0065636e, 0x00030005, 0x00000018, 0x00000000, 0x00060005,
    0x0000001c, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00030047,
    0x00000016, 0x00000002, 0x00050048, 0x00000016, 0x00000000, 0x0000000b,
    0x00000000, 0x00050048, 0x00000016, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x00000016, 0x00000002, 0x0000000b, 0x00000003, 0x00050048,
    0x00000016, 0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x0000001c,
    0x0000000b, 0x0000002a, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000002, 0x00040015, 0x00000008, 0x00000020, 0x00000000,
    0x0004002b, 0x00000008, 0x00000009, 0x00000003, 0x0004001c, 0x0000000a,
    0x00000007, 0x00000009, 0x00040020, 0x0000000b, 0x00000007, 0x0000000a,
    0x0004002b, 0x00000006, 0x0000000d, 0xbf800000, 0x0005002c, 0x00000007,
    0x0000000e, 0x0000000d, 0x0000000d, 0x0004002b, 0x00000006, 0x0000000f,
    0x40400000, 0x0005002c, 0x00000007, 0x00000010, 0x0000000f, 0x0000000d,
    0x0005002c, 0x00000007, 0x00000011, 0x0000000d, 0x0000000f, 0x0006002c,
    0x0000000a, 0x00000012, 0x0000000e, 0x00000010, 0x00000011, 0x00040017,
    0x00000013, 0x00000006, 0x00000004, 0x0004002b, 0x00000008, 0x00000014,
    0x00000001, 0x0004001c, 0x00000015, 0x00000006, 0x00000014, 0x0006001e,
    0x00000016, 0x00000013, 0x00000006, 0x00000015, 0x00000015, 0x00040020,
    0x00000017, 0x00000003, 0x00000016, 0x0004003b, 0x00000017, 0x00000018,
    0x00000003, 0x00040015, 0x00000019, 0x00000020, 0x00000001, 0x0004002b,
    0x00000019, 0x0000001a, 0x00000000, 0x00040020, 0x0000001b, 0x00000001,
    0x00000019, 0x0004003b, 0x0000001b, 0x0000001c, 0x00000001, 0x00040020,
    0x0000001e, 0x00000007, 0x00000007, 0x0004002b, 0x00000006, 0x00000021,
    0x00000000, 0x0004002b, 0x00000006, 0x00000022, 0x3f800000, 0x00040020,
    0x00000026, 0x00000003, 0x00000013, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x0000000b,
    0x0000000c, 0x00000007, 0x0003003e, 0x0000000c, 0x00000012, 0x0004003d,
    0x00000019, 0x0000001d, 0x0000001c, 0x00050041, 0x0000001e, 0x0000001f,
    0x0000000c, 0x0000001d, 0x0004003d, 0x00000007, 0x00000020, 0x0000001f,
    0x00050051, 0x00000006, 0x00000023, 0x00000020, 0x00000000, 0x00050051,
    0x00000006, 0x00000024, 0x00000020, 0x00000001, 0x00070050, 0x00000013,
    0x00000025, 0x00000023, 0x00000024, 0x00000021, 0x00000022, 0x00050041,
    0x00000026, 0x00000027, 0x00000018, 0x0000001a, 0x0003003e, 0x00000027,
    0x00000025, 0x000100fd, 0x00010038,
};

// SPIR-V for vertex_input.vert. The vertex shader consumes one vec2 from
// location 0 so the test exercises a real Vulkan vertex-buffer binding.
static constexpr uint32_t kVertexInputSpirv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000001b, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000012, 0x00030003,
    0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47, 0x4c474f4f, 0x70635f45,
    0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f, 0x69746365, 0x00006576,
    0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45, 0x64756c63, 0x69645f65,
    0x74636572, 0x00657669, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000,
    0x00060005, 0x0000000b, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000,
    0x00060006, 0x0000000b, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69,
    0x00070006, 0x0000000b, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953,
    0x00000000, 0x00070006, 0x0000000b, 0x00000002, 0x435f6c67, 0x4470696c,
    0x61747369, 0x0065636e, 0x00070006, 0x0000000b, 0x00000003, 0x435f6c67,
    0x446c6c75, 0x61747369, 0x0065636e, 0x00030005, 0x0000000d, 0x00000000,
    0x00050005, 0x00000012, 0x6f506e69, 0x69746973, 0x00006e6f, 0x00030047,
    0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b,
    0x00000000, 0x00050048, 0x0000000b, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000000b, 0x00000002, 0x0000000b, 0x00000003, 0x00050048,
    0x0000000b, 0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x00000012,
    0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003,
    0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040015, 0x00000008, 0x00000020, 0x00000000,
    0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c, 0x0000000a,
    0x00000006, 0x00000009, 0x0006001e, 0x0000000b, 0x00000007, 0x00000006,
    0x0000000a, 0x0000000a, 0x00040020, 0x0000000c, 0x00000003, 0x0000000b,
    0x0004003b, 0x0000000c, 0x0000000d, 0x00000003, 0x00040015, 0x0000000e,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000,
    0x00040017, 0x00000010, 0x00000006, 0x00000002, 0x00040020, 0x00000011,
    0x00000001, 0x00000010, 0x0004003b, 0x00000011, 0x00000012, 0x00000001,
    0x0004002b, 0x00000006, 0x00000014, 0x00000000, 0x0004002b, 0x00000006,
    0x00000015, 0x3f800000, 0x00040020, 0x00000019, 0x00000003, 0x00000007,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0004003d, 0x00000010, 0x00000013, 0x00000012, 0x00050051,
    0x00000006, 0x00000016, 0x00000013, 0x00000000, 0x00050051, 0x00000006,
    0x00000017, 0x00000013, 0x00000001, 0x00070050, 0x00000007, 0x00000018,
    0x00000016, 0x00000017, 0x00000014, 0x00000015, 0x00050041, 0x00000019,
    0x0000001a, 0x0000000d, 0x0000000f, 0x0003003e, 0x0000001a, 0x00000018,
    0x000100fd, 0x00010038,
};

static constexpr uint32_t kRenderSmokeFragmentSpirv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000000f, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x4374756f, 0x726f6c6f,
    0x00000000, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006,
    0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020,
    0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009,
    0x00000003, 0x0004002b, 0x00000006, 0x0000000a, 0x3e800000, 0x0004002b,
    0x00000006, 0x0000000b, 0x3f000000, 0x0004002b, 0x00000006, 0x0000000c,
    0x3f400000, 0x0004002b, 0x00000006, 0x0000000d, 0x3f800000, 0x0007002c,
    0x00000007, 0x0000000e, 0x0000000a, 0x0000000b, 0x0000000c, 0x0000000d,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0003003e, 0x00000009, 0x0000000e, 0x000100fd, 0x00010038,
};

// SPIR-V for descriptor_uniform.frag. The fragment shader reads one vec4 from
// set 0 / binding 0 so the test exercises a real Metal argument-buffer binding.
static constexpr uint32_t kDescriptorUniformFragmentSpirv[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x00000012, 0x00000000, 0x00020011,
    0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00030010, 0x00000004,
    0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x475f4c47,
    0x4c474f4f, 0x70635f45, 0x74735f70, 0x5f656c79, 0x656e696c, 0x7269645f,
    0x69746365, 0x00006576, 0x00080004, 0x475f4c47, 0x4c474f4f, 0x6e695f45,
    0x64756c63, 0x69645f65, 0x74636572, 0x00657669, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x4374756f, 0x726f6c6f,
    0x00000000, 0x00050005, 0x0000000a, 0x6f6c6f43, 0x6f6c4272, 0x00006b63,
    0x00050006, 0x0000000a, 0x00000000, 0x756c6176, 0x00000065, 0x00050005,
    0x0000000c, 0x6f6c6f63, 0x6f6c4272, 0x00006b63, 0x00040047, 0x00000009,
    0x0000001e, 0x00000000, 0x00030047, 0x0000000a, 0x00000002, 0x00050048,
    0x0000000a, 0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x0000000c,
    0x00000021, 0x00000000, 0x00040047, 0x0000000c, 0x00000022, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016,
    0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004,
    0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008,
    0x00000009, 0x00000003, 0x0003001e, 0x0000000a, 0x00000007, 0x00040020,
    0x0000000b, 0x00000002, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c,
    0x00000002, 0x00040015, 0x0000000d, 0x00000020, 0x00000001, 0x0004002b,
    0x0000000d, 0x0000000e, 0x00000000, 0x00040020, 0x0000000f, 0x00000002,
    0x00000007, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x00050041, 0x0000000f, 0x00000010, 0x0000000c,
    0x0000000e, 0x0004003d, 0x00000007, 0x00000011, 0x00000010, 0x0003003e,
    0x00000009, 0x00000011, 0x000100fd, 0x00010038,
};


VkShaderModule createShaderModule(VkDevice device,
                                  const uint32_t* code,
                                  size_t codeSize,
                                  const char* operation) {
    VkShaderModuleCreateInfo createInfo = makeVkStruct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    createInfo.codeSize = codeSize;
    createInfo.pCode = code;
    VkShaderModule module = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device, &createInfo, nullptr, &module), operation);
    return module;
}

}  // namespace

int main() {
    try {
        uint32_t instanceExtensionCount = 0;
        check(vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr),
              "vkEnumerateInstanceExtensionProperties(count)");
        std::vector<VkExtensionProperties> instanceExtensions(instanceExtensionCount);
        check(vkEnumerateInstanceExtensionProperties(
                  nullptr, &instanceExtensionCount, instanceExtensions.data()),
              "vkEnumerateInstanceExtensionProperties(list)");

        std::vector<const char*> enabledInstanceExtensions;
        VkInstanceCreateFlags instanceFlags = 0;
        constexpr const char* kPortabilityEnumeration = "VK_KHR_portability_enumeration";
        if (hasExtension(instanceExtensions, kPortabilityEnumeration)) {
            enabledInstanceExtensions.push_back(kPortabilityEnumeration);
            instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }

        VkApplicationInfo applicationInfo = makeVkStruct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        applicationInfo.pApplicationName = "MoltenVK Metal 4 Phase 1C e2e";
        applicationInfo.apiVersion = VK_API_VERSION_1_3;

        VkInstanceCreateInfo instanceCreateInfo = makeVkStruct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        instanceCreateInfo.flags = instanceFlags;
        instanceCreateInfo.pApplicationInfo = &applicationInfo;
        instanceCreateInfo.enabledExtensionCount =
            static_cast<uint32_t>(enabledInstanceExtensions.size());
        instanceCreateInfo.ppEnabledExtensionNames = enabledInstanceExtensions.data();

        VkInstance instance = VK_NULL_HANDLE;
        check(vkCreateInstance(&instanceCreateInfo, nullptr, &instance), "vkCreateInstance");

        uint32_t physicalDeviceCount = 0;
        check(vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr),
              "vkEnumeratePhysicalDevices(count)");
        if (physicalDeviceCount == 0) { fail("No Vulkan physical device"); }
        std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
        check(vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data()),
              "vkEnumeratePhysicalDevices(list)");
        VkPhysicalDevice physicalDevice = physicalDevices.front();

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(
            physicalDevice, &queueFamilyCount, queueFamilies.data());
        uint32_t queueFamilyIndex = UINT32_MAX;
        for (uint32_t index = 0; index < queueFamilyCount; ++index) {
            const VkQueueFlags required =
                VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((queueFamilies[index].queueFlags & required) == required) {
                queueFamilyIndex = index;
                break;
            }
        }
        if (queueFamilyIndex == UINT32_MAX) { fail("No Vulkan graphics+transfer+compute queue"); }

        uint32_t deviceExtensionCount = 0;
        check(vkEnumerateDeviceExtensionProperties(
                  physicalDevice, nullptr, &deviceExtensionCount, nullptr),
              "vkEnumerateDeviceExtensionProperties(count)");
        std::vector<VkExtensionProperties> deviceExtensions(deviceExtensionCount);
        check(vkEnumerateDeviceExtensionProperties(
                  physicalDevice, nullptr, &deviceExtensionCount, deviceExtensions.data()),
              "vkEnumerateDeviceExtensionProperties(list)");
        std::vector<const char*> enabledDeviceExtensions;
        constexpr const char* kPortabilitySubset = "VK_KHR_portability_subset";
        if (hasExtension(deviceExtensions, kPortabilitySubset)) {
            enabledDeviceExtensions.push_back(kPortabilitySubset);
        }
        constexpr const char* kDynamicRendering = "VK_KHR_dynamic_rendering";
        if (hasExtension(deviceExtensions, kDynamicRendering)) {
            enabledDeviceExtensions.push_back(kDynamicRendering);
        }
        constexpr const char* kExtendedDynamicState = "VK_EXT_extended_dynamic_state";
        if (!hasExtension(deviceExtensions, kExtendedDynamicState)) {
            fail("Extended dynamic state is unavailable");
        }
        enabledDeviceExtensions.push_back(kExtendedDynamicState);

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo = makeVkStruct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &priority;

        VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures = makeVkStruct<VkPhysicalDeviceDynamicRenderingFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT extendedDynamicStateFeatures =
            makeVkStruct<VkPhysicalDeviceExtendedDynamicStateFeaturesEXT>(
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT);
        dynamicRenderingFeatures.pNext = &extendedDynamicStateFeatures;
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures = makeVkStruct<VkPhysicalDeviceTimelineSemaphoreFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        timelineFeatures.pNext = &dynamicRenderingFeatures;
        VkPhysicalDeviceFeatures2 supportedFeatures = makeVkStruct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        supportedFeatures.pNext = &timelineFeatures;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &supportedFeatures);
        if (!timelineFeatures.timelineSemaphore) { fail("Timeline semaphores are unavailable"); }
        if (!dynamicRenderingFeatures.dynamicRendering) { fail("Dynamic rendering is unavailable"); }
        if (!extendedDynamicStateFeatures.extendedDynamicState) {
            fail("Extended dynamic state feature is unavailable");
        }
        if (!supportedFeatures.features.multiViewport) {
            fail("Multiple viewports are unavailable");
        }
        timelineFeatures.timelineSemaphore = VK_TRUE;
        dynamicRenderingFeatures.dynamicRendering = VK_TRUE;
        extendedDynamicStateFeatures.extendedDynamicState = VK_TRUE;
        VkPhysicalDeviceFeatures enabledFeatures{};
        enabledFeatures.multiViewport = VK_TRUE;

        VkDeviceCreateInfo deviceCreateInfo = makeVkStruct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        deviceCreateInfo.pNext = &timelineFeatures;
        deviceCreateInfo.queueCreateInfoCount = 1;
        deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
        deviceCreateInfo.pEnabledFeatures = &enabledFeatures;
        deviceCreateInfo.enabledExtensionCount =
            static_cast<uint32_t>(enabledDeviceExtensions.size());
        deviceCreateInfo.ppEnabledExtensionNames = enabledDeviceExtensions.data();

        VkDevice device = VK_NULL_HANDLE;
        check(vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device),
              "vkCreateDevice");
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

        VkCommandPoolCreateInfo poolCreateInfo = makeVkStruct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        poolCreateInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        poolCreateInfo.queueFamilyIndex = queueFamilyIndex;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        check(vkCreateCommandPool(device, &poolCreateInfo, nullptr, &commandPool),
              "vkCreateCommandPool");

        constexpr VkDeviceSize kSize = 4096;
        Buffer a = createBuffer(physicalDevice, device, kSize);
        Buffer b = createBuffer(physicalDevice, device, kSize);
        Buffer c = createBuffer(physicalDevice, device, kSize);

        // MTL4 fill -> legacy barrier/copy -> MTL4 copy. The final readback proves
        // cross-backend total order, whole-submission fallback, and MTL4 execution.
        VkCommandBuffer fillA = beginCommandBuffer(device, commandPool);
        vkCmdFillBuffer(fillA, a.buffer, 0, kSize, 0x5A5A5A5Au);
        endCommandBuffer(fillA);

        VkCommandBuffer fallbackCopy = beginCommandBuffer(device, commandPool);
        VkBufferMemoryBarrier barrier = makeVkStruct<VkBufferMemoryBarrier>(VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = a.buffer;
        barrier.offset = 0;
        barrier.size = kSize;
        vkCmdPipelineBarrier(fallbackCopy,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0,
                             nullptr,
                             1,
                             &barrier,
                             0,
                             nullptr);
        VkBufferCopy copyRegion{0, 0, kSize};
        vkCmdCopyBuffer(fallbackCopy, a.buffer, b.buffer, 1, &copyRegion);
        endCommandBuffer(fallbackCopy);

        VkCommandBuffer copyToReadback = beginCommandBuffer(device, commandPool);
        vkCmdCopyBuffer(copyToReadback, b.buffer, c.buffer, 1, &copyRegion);
        endCommandBuffer(copyToReadback);

        std::array<VkSubmitInfo, 3> orderedSubmits{};
        for (auto& submit : orderedSubmits) { submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; }
        orderedSubmits[0].commandBufferCount = 1;
        orderedSubmits[0].pCommandBuffers = &fillA;
        orderedSubmits[1].commandBufferCount = 1;
        orderedSubmits[1].pCommandBuffers = &fallbackCopy;
        orderedSubmits[2].commandBufferCount = 1;
        orderedSubmits[2].pCommandBuffers = &copyToReadback;
        VkFence orderedFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(orderedSubmits.size()),
                            orderedSubmits.data(), orderedFence),
              "vkQueueSubmit(hybrid order)");
        waitFence(device, orderedFence);
        validateRepeatedByte(device, c, 0x5A);
        vkDestroyFence(device, orderedFence, nullptr);

        // Explicit binary semaphore path: both submissions are eligible for MTL4.
        VkCommandBuffer fillWithSemaphore = beginCommandBuffer(device, commandPool);
        vkCmdFillBuffer(fillWithSemaphore, a.buffer, 0, kSize, 0x3C3C3C3Cu);
        endCommandBuffer(fillWithSemaphore);
        VkCommandBuffer copyWithSemaphore = beginCommandBuffer(device, commandPool);
        vkCmdCopyBuffer(copyWithSemaphore, a.buffer, c.buffer, 1, &copyRegion);
        endCommandBuffer(copyWithSemaphore);

        VkSemaphoreCreateInfo semaphoreCreateInfo = makeVkStruct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        VkSemaphore semaphore = VK_NULL_HANDLE;
        check(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &semaphore),
              "vkCreateSemaphore");
        VkSubmitInfo signalSubmit = makeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        signalSubmit.commandBufferCount = 1;
        signalSubmit.pCommandBuffers = &fillWithSemaphore;
        signalSubmit.signalSemaphoreCount = 1;
        signalSubmit.pSignalSemaphores = &semaphore;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo waitSubmit = makeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        waitSubmit.waitSemaphoreCount = 1;
        waitSubmit.pWaitSemaphores = &semaphore;
        waitSubmit.pWaitDstStageMask = &waitStage;
        waitSubmit.commandBufferCount = 1;
        waitSubmit.pCommandBuffers = &copyWithSemaphore;
        VkFence semaphoreFence = createFence(device);
        check(vkQueueSubmit(queue, 1, &signalSubmit, VK_NULL_HANDLE),
              "vkQueueSubmit(signal semaphore)");
        check(vkQueueSubmit(queue, 1, &waitSubmit, semaphoreFence),
              "vkQueueSubmit(wait semaphore)");
        waitFence(device, semaphoreFence);
        validateRepeatedByte(device, c, 0x3C);

        // Timeline semaphore values must survive the Metal 4 queue bridge.
        VkSemaphoreTypeCreateInfo timelineType = makeVkStruct<VkSemaphoreTypeCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO);
        timelineType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timelineType.initialValue = 0;
        VkSemaphoreCreateInfo timelineCreateInfo = makeVkStruct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        timelineCreateInfo.pNext = &timelineType;
        VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
        check(vkCreateSemaphore(device, &timelineCreateInfo, nullptr, &timelineSemaphore),
              "vkCreateSemaphore(timeline)");

        VkCommandBuffer timelineFill = beginCommandBuffer(device, commandPool);
        vkCmdFillBuffer(timelineFill, a.buffer, 0, kSize, 0x7D7D7D7Du);
        endCommandBuffer(timelineFill);
        VkCommandBuffer timelineCopy = beginCommandBuffer(device, commandPool);
        vkCmdCopyBuffer(timelineCopy, a.buffer, c.buffer, 1, &copyRegion);
        endCommandBuffer(timelineCopy);

        const uint64_t timelineValue = 7;
        VkTimelineSemaphoreSubmitInfo timelineSignalInfo = makeVkStruct<VkTimelineSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        timelineSignalInfo.signalSemaphoreValueCount = 1;
        timelineSignalInfo.pSignalSemaphoreValues = &timelineValue;
        VkSubmitInfo timelineSignalSubmit = makeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        timelineSignalSubmit.pNext = &timelineSignalInfo;
        timelineSignalSubmit.commandBufferCount = 1;
        timelineSignalSubmit.pCommandBuffers = &timelineFill;
        timelineSignalSubmit.signalSemaphoreCount = 1;
        timelineSignalSubmit.pSignalSemaphores = &timelineSemaphore;

        VkTimelineSemaphoreSubmitInfo timelineWaitInfo = makeVkStruct<VkTimelineSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO);
        timelineWaitInfo.waitSemaphoreValueCount = 1;
        timelineWaitInfo.pWaitSemaphoreValues = &timelineValue;
        VkSubmitInfo timelineWaitSubmit = makeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        timelineWaitSubmit.pNext = &timelineWaitInfo;
        timelineWaitSubmit.waitSemaphoreCount = 1;
        timelineWaitSubmit.pWaitSemaphores = &timelineSemaphore;
        timelineWaitSubmit.pWaitDstStageMask = &waitStage;
        timelineWaitSubmit.commandBufferCount = 1;
        timelineWaitSubmit.pCommandBuffers = &timelineCopy;
        VkFence timelineFence = createFence(device);
        check(vkQueueSubmit(queue, 1, &timelineSignalSubmit, VK_NULL_HANDLE),
              "vkQueueSubmit(signal timeline)");
        check(vkQueueSubmit(queue, 1, &timelineWaitSubmit, timelineFence),
              "vkQueueSubmit(wait timeline)");
        waitFence(device, timelineFence);
        validateRepeatedByte(device, c, 0x7D);
        std::cout << "TIMELINE_OK" << std::endl;

        // A descriptorless compute pipeline exercises the real MTL4 dispatch path.
        VkShaderModule computeModule = createDescriptorlessComputeShader(device);
        VkPipelineLayoutCreateInfo pipelineLayoutInfo = makeVkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        VkPipelineLayout computeLayout = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &computeLayout),
              "vkCreatePipelineLayout(compute)");
        VkPipelineShaderStageCreateInfo computeStage = makeVkStruct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
        computeStage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        computeStage.module = computeModule;
        computeStage.pName = "main";
        VkComputePipelineCreateInfo computePipelineInfo = makeVkStruct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
        computePipelineInfo.stage = computeStage;
        computePipelineInfo.layout = computeLayout;
        VkPipeline computePipeline = VK_NULL_HANDLE;
        check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                                       &computePipelineInfo, nullptr, &computePipeline),
              "vkCreateComputePipelines");
        VkCommandBuffer computeCommand = beginCommandBuffer(device, commandPool);
        vkCmdBindPipeline(computeCommand, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
        vkCmdDispatch(computeCommand, 4, 2, 1);
        endCommandBuffer(computeCommand);
        VkSubmitInfo computeSubmit = makeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        computeSubmit.commandBufferCount = 1;
        computeSubmit.pCommandBuffers = &computeCommand;
        VkFence computeFence = createFence(device);
        check(vkQueueSubmit(queue, 1, &computeSubmit, computeFence),
              "vkQueueSubmit(compute)");
        waitFence(device, computeFence);
        std::cout << "COMPUTE_OK" << std::endl;

        // Legacy upload -> MTL4 image copy -> legacy readback proves image data
        // and both directions of hybrid queue ordering.
        constexpr uint32_t kImageWidth = 8;
        constexpr uint32_t kImageHeight = 8;
        constexpr VkDeviceSize kImageBytes = kImageWidth * kImageHeight * 4;
        Buffer imageUpload = createBuffer(physicalDevice, device, kImageBytes);
        Buffer imageReadback = createBuffer(physicalDevice, device, kImageBytes);
        Image srcImage = createImage(physicalDevice, device, kImageWidth, kImageHeight);
        Image dstImage = createImage(physicalDevice, device, kImageWidth, kImageHeight);
        std::vector<uint8_t> imagePattern(static_cast<size_t>(kImageBytes));
        for (size_t index = 0; index < imagePattern.size(); ++index) {
            imagePattern[index] = static_cast<uint8_t>((index * 37u + 11u) & 0xffu);
        }
        writeBytes(device, imageUpload, imagePattern);

        VkBufferImageCopy imageRegion{};
        imageRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageRegion.imageSubresource.mipLevel = 0;
        imageRegion.imageSubresource.baseArrayLayer = 0;
        imageRegion.imageSubresource.layerCount = 1;
        imageRegion.imageExtent = {kImageWidth, kImageHeight, 1};

        VkCommandBuffer uploadImage = beginCommandBuffer(device, commandPool);
        imageBarrier(uploadImage, srcImage.image,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(uploadImage, dstImage.image,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyBufferToImage(uploadImage, imageUpload.buffer, srcImage.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageRegion);
        imageBarrier(uploadImage, srcImage.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(uploadImage);

        VkImageCopy copyImageRegion{};
        copyImageRegion.srcSubresource = imageRegion.imageSubresource;
        copyImageRegion.dstSubresource = imageRegion.imageSubresource;
        copyImageRegion.extent = imageRegion.imageExtent;
        VkCommandBuffer copyImage = beginCommandBuffer(device, commandPool);
        vkCmdCopyImage(copyImage,
                       srcImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dstImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &copyImageRegion);
        endCommandBuffer(copyImage);

        VkCommandBuffer readImage = beginCommandBuffer(device, commandPool);
        imageBarrier(readImage, dstImage.image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(readImage, dstImage.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               imageReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readImage);

        std::array<VkSubmitInfo, 3> imageSubmits{};
        for (auto& submit : imageSubmits) { submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; }
        imageSubmits[0].commandBufferCount = 1;
        imageSubmits[0].pCommandBuffers = &uploadImage;
        imageSubmits[1].commandBufferCount = 1;
        imageSubmits[1].pCommandBuffers = &copyImage;
        imageSubmits[2].commandBufferCount = 1;
        imageSubmits[2].pCommandBuffers = &readImage;
        VkFence imageFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(imageSubmits.size()),
                            imageSubmits.data(), imageFence),
              "vkQueueSubmit(image sequence)");
        waitFence(device, imageFence);
        validateBytes(device, imageReadback, imagePattern);
        std::cout << "IMAGE_DATA_OK" << std::endl;

        // VK_IMAGE_LAYOUT_GENERAL is the dominant Ryujinx image-barrier shape.
        // Transition the already-populated color image to GENERAL and consume it
        // in the same MTL4 command buffer so the marker proves both layout-state
        // publication and real image data, rather than telemetry alone.
        VkCommandBuffer generalImage = beginCommandBuffer(device, commandPool);
        imageBarrier(generalImage, dstImage.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(generalImage, dstImage.image,
                               VK_IMAGE_LAYOUT_GENERAL,
                               imageReadback.buffer, 1, &imageRegion);
        endCommandBuffer(generalImage);
        VkFence generalImageFence = createFence(device);
        VkSubmitInfo generalImageSubmit = makeVkStruct<VkSubmitInfo>(
            VK_STRUCTURE_TYPE_SUBMIT_INFO);
        generalImageSubmit.commandBufferCount = 1;
        generalImageSubmit.pCommandBuffers = &generalImage;
        check(vkQueueSubmit(queue, 1, &generalImageSubmit, generalImageFence),
              "vkQueueSubmit(general image layout)");
        waitFence(device, generalImageFence);
        validateBytes(device, imageReadback, imagePattern);
        std::cout << "GENERAL_LAYOUT_IMAGE_OK" << std::endl;

        // A real descriptorless graphics pipeline, dynamic-rendering pass, and
        // non-indexed draw must execute through MTL4. A following legacy image
        // readback proves both rendered data and hybrid queue ordering.
        Image renderTarget = createImage(physicalDevice, device, kImageWidth, kImageHeight);
        Buffer renderReadback = createBuffer(physicalDevice, device, kImageBytes);
        VkShaderModule vertexModule = createShaderModule(
            device, kRenderSmokeVertexSpirv, sizeof(kRenderSmokeVertexSpirv),
            "vkCreateShaderModule(vertex)");
        VkShaderModule fragmentModule = createShaderModule(
            device, kRenderSmokeFragmentSpirv, sizeof(kRenderSmokeFragmentSpirv),
            "vkCreateShaderModule(fragment)");

        VkPipelineLayoutCreateInfo renderLayoutInfo = makeVkStruct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        VkPipelineLayout renderLayout = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(device, &renderLayoutInfo, nullptr, &renderLayout),
              "vkCreatePipelineLayout(render)");

        std::array<VkPipelineShaderStageCreateInfo, 2> renderStages{};
        for (auto& stage : renderStages) {
            stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stage.pName = "main";
        }
        renderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        renderStages[0].module = vertexModule;
        renderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        renderStages[1].module = fragmentModule;

        VkPipelineVertexInputStateCreateInfo vertexInput = makeVkStruct<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        VkPipelineInputAssemblyStateCreateInfo inputAssembly = makeVkStruct<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(kImageWidth),
                            static_cast<float>(kImageHeight), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, {kImageWidth, kImageHeight}};
        VkPipelineViewportStateCreateInfo viewportState = makeVkStruct<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewportState.viewportCount = 1;
        viewportState.pViewports = &viewport;
        viewportState.scissorCount = 1;
        viewportState.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rasterization = makeVkStruct<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        rasterization.depthClampEnable = VK_FALSE;
        rasterization.rasterizerDiscardEnable = VK_FALSE;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rasterization.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample = makeVkStruct<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                         VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT |
                                         VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blendState = makeVkStruct<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blendState.attachmentCount = 1;
        blendState.pAttachments = &blendAttachment;
        VkPipelineRenderingCreateInfo pipelineRendering = makeVkStruct<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
        pipelineRendering.colorAttachmentCount = 1;
        pipelineRendering.pColorAttachmentFormats = &renderTarget.format;
        VkGraphicsPipelineCreateInfo graphicsInfo = makeVkStruct<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        graphicsInfo.pNext = &pipelineRendering;
        graphicsInfo.stageCount = static_cast<uint32_t>(renderStages.size());
        graphicsInfo.pStages = renderStages.data();
        graphicsInfo.pVertexInputState = &vertexInput;
        graphicsInfo.pInputAssemblyState = &inputAssembly;
        graphicsInfo.pViewportState = &viewportState;
        graphicsInfo.pRasterizationState = &rasterization;
        graphicsInfo.pMultisampleState = &multisample;
        graphicsInfo.pColorBlendState = &blendState;
        graphicsInfo.layout = renderLayout;
        graphicsInfo.renderPass = VK_NULL_HANDLE;
        VkPipeline graphicsPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &graphicsPipeline),
              "vkCreateGraphicsPipelines(render)");

        VkRenderingAttachmentInfo colorAttachment = makeVkStruct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
        colorAttachment.imageView = renderTarget.view;
        colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAttachment.resolveMode = VK_RESOLVE_MODE_NONE;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo renderingInfo = makeVkStruct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
        renderingInfo.renderArea = scissor;
        renderingInfo.layerCount = 1;
        renderingInfo.colorAttachmentCount = 1;
        renderingInfo.pColorAttachments = &colorAttachment;

        // Keep image-layout side effects on the legacy path. The isolated draw
        // submission can then prove real MTL4 rendering without asking the
        // experimental barrier materializer to mutate MoltenVK layout state.
        VkCommandBuffer prepareRender = beginCommandBuffer(device, commandPool);
        imageBarrier(prepareRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        endCommandBuffer(prepareRender);

        VkCommandBuffer renderCommand = beginCommandBuffer(device, commandPool);
        vkCmdBeginRendering(renderCommand, &renderingInfo);
        vkCmdBindPipeline(renderCommand, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        vkCmdDraw(renderCommand, 3, 1, 0, 0);
        vkCmdEndRendering(renderCommand);
        endCommandBuffer(renderCommand);

        VkCommandBuffer finishRender = beginCommandBuffer(device, commandPool);
        imageBarrier(finishRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishRender);

        VkCommandBuffer readRender = beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readRender, renderTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               renderReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readRender);
        std::array<VkSubmitInfo, 4> renderSubmits{};
        for (auto& submit : renderSubmits) { submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO; }
        renderSubmits[0].commandBufferCount = 1;
        renderSubmits[0].pCommandBuffers = &prepareRender;
        renderSubmits[1].commandBufferCount = 1;
        renderSubmits[1].pCommandBuffers = &renderCommand;
        renderSubmits[2].commandBufferCount = 1;
        renderSubmits[2].pCommandBuffers = &finishRender;
        renderSubmits[3].commandBufferCount = 1;
        renderSubmits[3].pCommandBuffers = &readRender;
        VkFence renderFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(renderSubmits.size()),
                            renderSubmits.data(), renderFence),
              "vkQueueSubmit(render sequence)");
        waitFence(device, renderFence);
        validateSolidColor(device, renderReadback, {64, 128, 191, 255});
        std::cout << "RENDER_OK" << std::endl;

        // The common descriptor path reuses MoltenVK's Metal 3 argument-buffer
        // encoding and exposes it through an MTL4 argument table. This must be a
        // real MTL4 render submission, not a correctness-preserving fallback.
        Buffer uniformColor = createBuffer(physicalDevice, device, sizeof(float) * 4);
        const std::array<float, 4> descriptorColor{{0.1f, 0.2f, 0.3f, 1.0f}};
        std::vector<uint8_t> descriptorColorBytes(sizeof(descriptorColor));
        std::memcpy(descriptorColorBytes.data(), descriptorColor.data(), descriptorColorBytes.size());
        writeBytes(device, uniformColor, descriptorColorBytes);

        VkDescriptorSetLayoutBinding uniformBinding{};
        uniformBinding.binding = 0;
        uniformBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uniformBinding.descriptorCount = 1;
        uniformBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo descriptorLayoutInfo =
            makeVkStruct<VkDescriptorSetLayoutCreateInfo>(
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
        descriptorLayoutInfo.bindingCount = 1;
        descriptorLayoutInfo.pBindings = &uniformBinding;
        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        check(vkCreateDescriptorSetLayout(device, &descriptorLayoutInfo, nullptr,
                                          &descriptorSetLayout),
              "vkCreateDescriptorSetLayout(uniform)");

        VkDescriptorPoolSize descriptorPoolSize{};
        descriptorPoolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorPoolSize.descriptorCount = 1;
        VkDescriptorPoolCreateInfo descriptorPoolInfo =
            makeVkStruct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        descriptorPoolInfo.maxSets = 1;
        descriptorPoolInfo.poolSizeCount = 1;
        descriptorPoolInfo.pPoolSizes = &descriptorPoolSize;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        check(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool),
              "vkCreateDescriptorPool(uniform)");
        VkDescriptorSetAllocateInfo descriptorAllocateInfo =
            makeVkStruct<VkDescriptorSetAllocateInfo>(
                VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        descriptorAllocateInfo.descriptorPool = descriptorPool;
        descriptorAllocateInfo.descriptorSetCount = 1;
        descriptorAllocateInfo.pSetLayouts = &descriptorSetLayout;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        check(vkAllocateDescriptorSets(device, &descriptorAllocateInfo, &descriptorSet),
              "vkAllocateDescriptorSets(uniform)");
        VkDescriptorBufferInfo uniformDescriptor{};
        uniformDescriptor.buffer = uniformColor.buffer;
        uniformDescriptor.offset = 0;
        uniformDescriptor.range = sizeof(float) * 4;
        VkWriteDescriptorSet descriptorWrite =
            makeVkStruct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        descriptorWrite.dstSet = descriptorSet;
        descriptorWrite.dstBinding = 0;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.pBufferInfo = &uniformDescriptor;
        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);

        VkPipelineLayoutCreateInfo descriptorPipelineLayoutInfo =
            makeVkStruct<VkPipelineLayoutCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        descriptorPipelineLayoutInfo.setLayoutCount = 1;
        descriptorPipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
        VkPipelineLayout descriptorPipelineLayout = VK_NULL_HANDLE;
        check(vkCreatePipelineLayout(device, &descriptorPipelineLayoutInfo, nullptr,
                                     &descriptorPipelineLayout),
              "vkCreatePipelineLayout(uniform)");
        VkShaderModule descriptorFragmentModule = createShaderModule(
            device, kDescriptorUniformFragmentSpirv,
            sizeof(kDescriptorUniformFragmentSpirv),
            "vkCreateShaderModule(uniform fragment)");
        renderStages[1].module = descriptorFragmentModule;
        graphicsInfo.layout = descriptorPipelineLayout;
        VkPipeline descriptorPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &descriptorPipeline),
              "vkCreateGraphicsPipelines(uniform)");

        VkCommandBuffer prepareDescriptorRender = beginCommandBuffer(device, commandPool);
        imageBarrier(prepareDescriptorRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        endCommandBuffer(prepareDescriptorRender);
        VkCommandBuffer descriptorRender = beginCommandBuffer(device, commandPool);
        vkCmdBeginRendering(descriptorRender, &renderingInfo);
        vkCmdBindPipeline(descriptorRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          descriptorPipeline);
        vkCmdBindDescriptorSets(descriptorRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                descriptorPipelineLayout, 0, 1, &descriptorSet,
                                0, nullptr);
        vkCmdDraw(descriptorRender, 3, 1, 0, 0);
        vkCmdEndRendering(descriptorRender);
        endCommandBuffer(descriptorRender);
        VkCommandBuffer finishDescriptorRender = beginCommandBuffer(device, commandPool);
        imageBarrier(finishDescriptorRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishDescriptorRender);
        VkCommandBuffer readDescriptorRender = beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readDescriptorRender, renderTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               renderReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readDescriptorRender);
        std::array<VkSubmitInfo, 4> descriptorSubmits{};
        for (auto& submit : descriptorSubmits) {
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        }
        descriptorSubmits[0].commandBufferCount = 1;
        descriptorSubmits[0].pCommandBuffers = &prepareDescriptorRender;
        descriptorSubmits[1].commandBufferCount = 1;
        descriptorSubmits[1].pCommandBuffers = &descriptorRender;
        descriptorSubmits[2].commandBufferCount = 1;
        descriptorSubmits[2].pCommandBuffers = &finishDescriptorRender;
        descriptorSubmits[3].commandBufferCount = 1;
        descriptorSubmits[3].pCommandBuffers = &readDescriptorRender;
        VkFence descriptorFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(descriptorSubmits.size()),
                            descriptorSubmits.data(), descriptorFence),
              "vkQueueSubmit(descriptor render sequence)");
        waitFence(device, descriptorFence);
        validateSolidColor(device, renderReadback, {26, 51, 77, 255});
        std::cout << "DESCRIPTOR_RENDER_OK" << std::endl;

        // A static vertex input must bind the VkBuffer at the Metal buffer
        // index compiled into the pipeline and snapshot that address through
        // the same MTL4 argument table used for descriptor resources.
        const std::array<float, 6> vertexPositions{{
            -1.0f, -1.0f,
             3.0f, -1.0f,
            -1.0f,  3.0f,
        }};
        Buffer vertexBuffer = createBuffer(physicalDevice, device, sizeof(vertexPositions));
        std::vector<uint8_t> vertexBytes(sizeof(vertexPositions));
        std::memcpy(vertexBytes.data(), vertexPositions.data(), vertexBytes.size());
        writeBytes(device, vertexBuffer, vertexBytes);

        VkVertexInputBindingDescription vertexBinding{};
        vertexBinding.binding = 0;
        vertexBinding.stride = sizeof(float) * 2;
        vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription vertexAttribute{};
        vertexAttribute.location = 0;
        vertexAttribute.binding = 0;
        vertexAttribute.format = VK_FORMAT_R32G32_SFLOAT;
        vertexAttribute.offset = 0;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &vertexBinding;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &vertexAttribute;
        VkShaderModule vertexInputModule = createShaderModule(
            device, kVertexInputSpirv, sizeof(kVertexInputSpirv),
            "vkCreateShaderModule(vertex input)");
        renderStages[0].module = vertexInputModule;
        renderStages[1].module = fragmentModule;
        graphicsInfo.layout = renderLayout;
        VkPipeline vertexInputPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &vertexInputPipeline),
              "vkCreateGraphicsPipelines(vertex input)");

        VkCommandBuffer prepareVertexRender = beginCommandBuffer(device, commandPool);
        imageBarrier(prepareVertexRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        endCommandBuffer(prepareVertexRender);
        VkCommandBuffer vertexRender = beginCommandBuffer(device, commandPool);
        vkCmdBeginRendering(vertexRender, &renderingInfo);
        vkCmdBindPipeline(vertexRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          vertexInputPipeline);
        VkDeviceSize vertexOffset = 0;
        vkCmdBindVertexBuffers(vertexRender, 0, 1, &vertexBuffer.buffer, &vertexOffset);
        vkCmdDraw(vertexRender, 3, 1, 0, 0);
        vkCmdEndRendering(vertexRender);
        endCommandBuffer(vertexRender);
        VkCommandBuffer finishVertexRender = beginCommandBuffer(device, commandPool);
        imageBarrier(finishVertexRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishVertexRender);
        VkCommandBuffer readVertexRender = beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readVertexRender, renderTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               renderReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readVertexRender);
        std::array<VkSubmitInfo, 4> vertexSubmits{};
        for (auto& submit : vertexSubmits) {
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        }
        vertexSubmits[0].commandBufferCount = 1;
        vertexSubmits[0].pCommandBuffers = &prepareVertexRender;
        vertexSubmits[1].commandBufferCount = 1;
        vertexSubmits[1].pCommandBuffers = &vertexRender;
        vertexSubmits[2].commandBufferCount = 1;
        vertexSubmits[2].pCommandBuffers = &finishVertexRender;
        vertexSubmits[3].commandBufferCount = 1;
        vertexSubmits[3].pCommandBuffers = &readVertexRender;
        VkFence vertexFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(vertexSubmits.size()),
                            vertexSubmits.data(), vertexFence),
              "vkQueueSubmit(vertex render sequence)");
        waitFence(device, vertexFence);
        validateSolidColor(device, renderReadback, {64, 128, 191, 255});
        std::cout << "VERTEX_RENDER_OK" << std::endl;

        // Ryujinx commonly declares the vertex binding stride dynamic and
        // supplies it with vkCmdBindVertexBuffers2. Metal 4 argument tables
        // carry that stride beside the GPU address for vertex-stage fetches.
        VkDynamicState dynamicVertexStride =
            VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
        VkPipelineDynamicStateCreateInfo dynamicVertexState =
            makeVkStruct<VkPipelineDynamicStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamicVertexState.dynamicStateCount = 1;
        dynamicVertexState.pDynamicStates = &dynamicVertexStride;
        graphicsInfo.pDynamicState = &dynamicVertexState;
        VkPipeline dynamicVertexPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &dynamicVertexPipeline),
              "vkCreateGraphicsPipelines(dynamic vertex stride)");

        VkCommandBuffer prepareDynamicVertexRender = beginCommandBuffer(device, commandPool);
        imageBarrier(prepareDynamicVertexRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        endCommandBuffer(prepareDynamicVertexRender);
        VkCommandBuffer dynamicVertexRender = beginCommandBuffer(device, commandPool);
        vkCmdBeginRendering(dynamicVertexRender, &renderingInfo);
        vkCmdBindPipeline(dynamicVertexRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dynamicVertexPipeline);
        VkDeviceSize vertexSize = sizeof(vertexPositions);
        VkDeviceSize dynamicStride = sizeof(float) * 2;
        vkCmdBindVertexBuffers2(dynamicVertexRender, 0, 1, &vertexBuffer.buffer,
                                &vertexOffset, &vertexSize, &dynamicStride);
        vkCmdDraw(dynamicVertexRender, 3, 1, 0, 0);
        vkCmdEndRendering(dynamicVertexRender);
        endCommandBuffer(dynamicVertexRender);
        VkCommandBuffer finishDynamicVertexRender = beginCommandBuffer(device, commandPool);
        imageBarrier(finishDynamicVertexRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishDynamicVertexRender);
        VkCommandBuffer readDynamicVertexRender = beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readDynamicVertexRender, renderTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               renderReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readDynamicVertexRender);
        std::array<VkSubmitInfo, 4> dynamicVertexSubmits{};
        for (auto& submit : dynamicVertexSubmits) {
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        }
        dynamicVertexSubmits[0].commandBufferCount = 1;
        dynamicVertexSubmits[0].pCommandBuffers = &prepareDynamicVertexRender;
        dynamicVertexSubmits[1].commandBufferCount = 1;
        dynamicVertexSubmits[1].pCommandBuffers = &dynamicVertexRender;
        dynamicVertexSubmits[2].commandBufferCount = 1;
        dynamicVertexSubmits[2].pCommandBuffers = &finishDynamicVertexRender;
        dynamicVertexSubmits[3].commandBufferCount = 1;
        dynamicVertexSubmits[3].pCommandBuffers = &readDynamicVertexRender;
        VkFence dynamicVertexFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(dynamicVertexSubmits.size()),
                            dynamicVertexSubmits.data(), dynamicVertexFence),
              "vkQueueSubmit(dynamic vertex render sequence)");
        waitFence(device, dynamicVertexFence);
        validateSolidColor(device, renderReadback, {64, 128, 191, 255});
        std::cout << "DYNAMIC_VERTEX_RENDER_OK" << std::endl;

        // The XC3 trace declares viewport and scissor dynamic on most otherwise
        // eligible graphics pipelines. Prove both commands independently: a
        // half-width viewport with a full scissor, then a full viewport with a
        // half-width scissor. Both must paint exactly the left half.
        std::array<VkDynamicState, 2> dynamicViewportScissorStates{{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
        }};
        VkPipelineDynamicStateCreateInfo dynamicViewportScissorState =
            makeVkStruct<VkPipelineDynamicStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamicViewportScissorState.dynamicStateCount =
            static_cast<uint32_t>(dynamicViewportScissorStates.size());
        dynamicViewportScissorState.pDynamicStates =
            dynamicViewportScissorStates.data();
        vertexInput.vertexBindingDescriptionCount = 0;
        vertexInput.pVertexBindingDescriptions = nullptr;
        vertexInput.vertexAttributeDescriptionCount = 0;
        vertexInput.pVertexAttributeDescriptions = nullptr;
        renderStages[0].module = vertexModule;
        viewportState.pViewports = nullptr;
        viewportState.pScissors = nullptr;
        graphicsInfo.pDynamicState = &dynamicViewportScissorState;
        VkPipeline dynamicViewportScissorPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &dynamicViewportScissorPipeline),
              "vkCreateGraphicsPipelines(dynamic viewport/scissor)");

        auto runDynamicViewportScissor = [&](VkPipeline pipeline,
                                             const VkViewport* dynamicViewports,
                                             uint32_t dynamicViewportCount,
                                             const VkRect2D* dynamicScissors,
                                             uint32_t dynamicScissorCount,
                                             bool setInactiveStencilState,
                                             bool setInactiveDepthBias,
                                             bool setActiveBlendConstants,
                                             const char* operation) {
            VkCommandBuffer prepare = beginCommandBuffer(device, commandPool);
            imageBarrier(prepare, renderTarget.image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            endCommandBuffer(prepare);

            VkCommandBuffer render = beginCommandBuffer(device, commandPool);
            vkCmdBeginRendering(render, &renderingInfo);
            vkCmdBindPipeline(render, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdSetViewport(render, 0, dynamicViewportCount, dynamicViewports);
            vkCmdSetScissor(render, 0, dynamicScissorCount, dynamicScissors);
            if (setInactiveStencilState) {
                vkCmdSetStencilCompareMask(render, VK_STENCIL_FACE_FRONT_BIT, 0x12);
                vkCmdSetStencilWriteMask(render, VK_STENCIL_FACE_FRONT_BIT, 0x34);
                vkCmdSetStencilReference(render, VK_STENCIL_FACE_FRONT_BIT, 0x56);
                vkCmdSetStencilCompareMask(render, VK_STENCIL_FACE_BACK_BIT, 0x78);
                vkCmdSetStencilWriteMask(render, VK_STENCIL_FACE_BACK_BIT, 0x9a);
                vkCmdSetStencilReference(render, VK_STENCIL_FACE_BACK_BIT, 0xbc);
            }
            if (setInactiveDepthBias) {
                vkCmdSetDepthBias(render, 7.0f, 3.0f, 5.0f);
            }
            if (setActiveBlendConstants) {
                const float blendConstants[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                vkCmdSetBlendConstants(render, blendConstants);
            }
            vkCmdDraw(render, 3, 1, 0, 0);
            vkCmdEndRendering(render);
            endCommandBuffer(render);

            VkCommandBuffer finish = beginCommandBuffer(device, commandPool);
            imageBarrier(finish, renderTarget.image,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT);
            endCommandBuffer(finish);
            VkCommandBuffer read = beginCommandBuffer(device, commandPool);
            vkCmdCopyImageToBuffer(read, renderTarget.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   renderReadback.buffer, 1, &imageRegion);
            endCommandBuffer(read);

            std::array<VkSubmitInfo, 4> submits{};
            for (auto& submit : submits) {
                submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            }
            submits[0].commandBufferCount = 1;
            submits[0].pCommandBuffers = &prepare;
            submits[1].commandBufferCount = 1;
            submits[1].pCommandBuffers = &render;
            submits[2].commandBufferCount = 1;
            submits[2].pCommandBuffers = &finish;
            submits[3].commandBufferCount = 1;
            submits[3].pCommandBuffers = &read;
            VkFence fence = createFence(device);
            check(vkQueueSubmit(queue, static_cast<uint32_t>(submits.size()),
                                submits.data(), fence), operation);
            waitFence(device, fence);
            validateLeftHalfColor(device, renderReadback, kImageWidth, kImageHeight,
                                  {64, 128, 191, 255});
            vkDestroyFence(device, fence, nullptr);
        };

        VkViewport halfViewport = viewport;
        halfViewport.width = static_cast<float>(kImageWidth / 2);
        runDynamicViewportScissor(dynamicViewportScissorPipeline,
                                  &halfViewport, 1, &scissor, 1, false, false, false,
                                  "vkQueueSubmit(dynamic viewport)");
        VkRect2D halfScissor = scissor;
        halfScissor.extent.width = kImageWidth / 2;
        runDynamicViewportScissor(dynamicViewportScissorPipeline,
                                  &viewport, 1, &halfScissor, 1, false, false, false,
                                  "vkQueueSubmit(dynamic scissor)");
        std::cout << "DYNAMIC_VIEWPORT_SCISSOR_OK" << std::endl;

        std::array<VkViewport, 2> multiViewports{{halfViewport, viewport}};
        std::array<VkRect2D, 2> multiScissors{{scissor, scissor}};
        viewportState.viewportCount = static_cast<uint32_t>(multiViewports.size());
        viewportState.scissorCount = static_cast<uint32_t>(multiScissors.size());
        VkPipeline multiViewportScissorPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &multiViewportScissorPipeline),
              "vkCreateGraphicsPipelines(multi viewport/scissor)");
        runDynamicViewportScissor(multiViewportScissorPipeline,
                                  multiViewports.data(),
                                  static_cast<uint32_t>(multiViewports.size()),
                                  multiScissors.data(),
                                  static_cast<uint32_t>(multiScissors.size()),
                                  false, false, false,
                                  "vkQueueSubmit(multi viewport/scissor)");
        std::cout << "MULTI_VIEWPORT_SCISSOR_OK" << std::endl;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        std::array<VkDynamicState, 3> activeBlendDynamicStates{{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        }};
        dynamicViewportScissorState.dynamicStateCount =
            static_cast<uint32_t>(activeBlendDynamicStates.size());
        dynamicViewportScissorState.pDynamicStates = activeBlendDynamicStates.data();
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipeline activeBlendConstantsPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &activeBlendConstantsPipeline),
              "vkCreateGraphicsPipelines(active blend constants)");
        runDynamicViewportScissor(activeBlendConstantsPipeline,
                                  &viewport, 1, &halfScissor, 1,
                                  false, false, true,
                                  "vkQueueSubmit(active blend constants)");
        std::cout << "ACTIVE_BLEND_CONSTANTS_OK" << std::endl;
        blendAttachment = {};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                         VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT |
                                         VK_COLOR_COMPONENT_A_BIT;

        std::array<VkDynamicState, 5> inactiveStencilDynamicStates{{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        }};
        dynamicViewportScissorState.dynamicStateCount =
            static_cast<uint32_t>(inactiveStencilDynamicStates.size());
        dynamicViewportScissorState.pDynamicStates =
            inactiveStencilDynamicStates.data();
        VkPipeline inactiveStencilDynamicPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &inactiveStencilDynamicPipeline),
              "vkCreateGraphicsPipelines(inactive dynamic stencil)");
        runDynamicViewportScissor(inactiveStencilDynamicPipeline,
                                  &viewport, 1, &halfScissor, 1, true, false, false,
                                  "vkQueueSubmit(inactive dynamic stencil)");
        std::cout << "INACTIVE_STENCIL_DYNAMIC_OK" << std::endl;

        std::array<VkDynamicState, 3> inactiveDepthBiasDynamicStates{{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR,
            VK_DYNAMIC_STATE_DEPTH_BIAS,
        }};
        dynamicViewportScissorState.dynamicStateCount =
            static_cast<uint32_t>(inactiveDepthBiasDynamicStates.size());
        dynamicViewportScissorState.pDynamicStates =
            inactiveDepthBiasDynamicStates.data();
        VkPipeline inactiveDepthBiasDynamicPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &inactiveDepthBiasDynamicPipeline),
              "vkCreateGraphicsPipelines(inactive dynamic depth bias)");
        runDynamicViewportScissor(inactiveDepthBiasDynamicPipeline,
                                  &viewport, 1, &halfScissor, 1, false, true, false,
                                  "vkQueueSubmit(inactive dynamic depth bias)");
        std::cout << "INACTIVE_DEPTH_BIAS_DYNAMIC_OK" << std::endl;

        viewportState.pViewports = &viewport;
        viewportState.pScissors = &scissor;
        graphicsInfo.pDynamicState = nullptr;

        // A depth attachment plus a static NEVER comparison must execute on
        // MTL4 without painting the color target. Leaving the MTL4 depth state
        // unbound would draw the fragment and make this readback fail.
        Image depthTarget = createImage(
            physicalDevice, device, kImageWidth, kImageHeight,
            VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
        VkPipelineDepthStencilStateCreateInfo depthStencilState =
            makeVkStruct<VkPipelineDepthStencilStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depthStencilState.depthTestEnable = VK_TRUE;
        depthStencilState.depthWriteEnable = VK_TRUE;
        depthStencilState.depthCompareOp = VK_COMPARE_OP_NEVER;
        pipelineRendering.depthAttachmentFormat = depthTarget.format;
        graphicsInfo.pDepthStencilState = &depthStencilState;
        graphicsInfo.pDynamicState = nullptr;
        renderStages[0].module = vertexModule;
        VkPipeline depthPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &depthPipeline),
              "vkCreateGraphicsPipelines(depth attachment)");

        VkRenderingAttachmentInfo depthAttachment =
            makeVkStruct<VkRenderingAttachmentInfo>(
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
        depthAttachment.imageView = depthTarget.view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.resolveMode = VK_RESOLVE_MODE_NONE;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo depthRenderingInfo = renderingInfo;
        depthRenderingInfo.pDepthAttachment = &depthAttachment;

        VkCommandBuffer prepareDepthRender = beginCommandBuffer(device, commandPool);
        imageBarrier(prepareDepthRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        imageBarrier(prepareDepthRender, depthTarget.image,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                     0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT);
        endCommandBuffer(prepareDepthRender);

        VkCommandBuffer depthRender = beginCommandBuffer(device, commandPool);
        vkCmdBeginRendering(depthRender, &depthRenderingInfo);
        vkCmdBindPipeline(depthRender, VK_PIPELINE_BIND_POINT_GRAPHICS, depthPipeline);
        vkCmdDraw(depthRender, 3, 1, 0, 0);
        vkCmdEndRendering(depthRender);
        endCommandBuffer(depthRender);

        VkCommandBuffer finishDepthRender = beginCommandBuffer(device, commandPool);
        imageBarrier(finishDepthRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishDepthRender);
        VkCommandBuffer readDepthRender = beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readDepthRender, renderTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               renderReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readDepthRender);

        std::array<VkSubmitInfo, 4> depthSubmits{};
        for (auto& submit : depthSubmits) {
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        }
        depthSubmits[0].commandBufferCount = 1;
        depthSubmits[0].pCommandBuffers = &prepareDepthRender;
        depthSubmits[1].commandBufferCount = 1;
        depthSubmits[1].pCommandBuffers = &depthRender;
        depthSubmits[2].commandBufferCount = 1;
        depthSubmits[2].pCommandBuffers = &finishDepthRender;
        depthSubmits[3].commandBufferCount = 1;
        depthSubmits[3].pCommandBuffers = &readDepthRender;
        VkFence depthFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(depthSubmits.size()),
                            depthSubmits.data(), depthFence),
              "vkQueueSubmit(depth render sequence)");
        waitFence(device, depthFence);
        validateSolidColor(device, renderReadback, {0, 0, 0, 255});
        std::cout << "DEPTH_RENDER_OK" << std::endl;

        // Ryujinx records its primary color pass with classic VkRenderPass
        // commands and multiple render targets. The fragment shader writes only
        // attachment zero; attachment one must retain its independent clear color.
        Image classicMrtTarget = createImage(
            physicalDevice, device, kImageWidth, kImageHeight);
        Buffer classicMrtReadback = createBuffer(
            physicalDevice, device, kImageBytes);
        std::array<VkAttachmentDescription, 2> classicColorDescriptions{};
        for (auto& description : classicColorDescriptions) {
            description.format = renderTarget.format;
            description.samples = VK_SAMPLE_COUNT_1_BIT;
            description.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            description.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            description.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            description.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            description.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            description.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        std::array<VkAttachmentReference, 2> classicColorReferences{};
        for (uint32_t index = 0; index < classicColorReferences.size(); ++index) {
            classicColorReferences[index].attachment = index;
            classicColorReferences[index].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        VkSubpassDescription classicSubpass{};
        classicSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        classicSubpass.colorAttachmentCount =
            static_cast<uint32_t>(classicColorReferences.size());
        classicSubpass.pColorAttachments = classicColorReferences.data();
        VkRenderPassCreateInfo classicRenderPassInfo =
            makeVkStruct<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        classicRenderPassInfo.attachmentCount =
            static_cast<uint32_t>(classicColorDescriptions.size());
        classicRenderPassInfo.pAttachments = classicColorDescriptions.data();
        classicRenderPassInfo.subpassCount = 1;
        classicRenderPassInfo.pSubpasses = &classicSubpass;
        VkRenderPass classicRenderPass = VK_NULL_HANDLE;
        check(vkCreateRenderPass(device, &classicRenderPassInfo, nullptr,
                                 &classicRenderPass),
              "vkCreateRenderPass(classic color)");

        VkFramebufferCreateInfo classicFramebufferInfo =
            makeVkStruct<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        classicFramebufferInfo.renderPass = classicRenderPass;
        std::array<VkImageView, 2> classicViews{{
            renderTarget.view, classicMrtTarget.view,
        }};
        classicFramebufferInfo.attachmentCount =
            static_cast<uint32_t>(classicViews.size());
        classicFramebufferInfo.pAttachments = classicViews.data();
        classicFramebufferInfo.width = kImageWidth;
        classicFramebufferInfo.height = kImageHeight;
        classicFramebufferInfo.layers = 1;
        VkFramebuffer classicFramebuffer = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(device, &classicFramebufferInfo, nullptr,
                                  &classicFramebuffer),
              "vkCreateFramebuffer(classic color)");

        graphicsInfo.pNext = nullptr;
        graphicsInfo.pDepthStencilState = nullptr;
        graphicsInfo.renderPass = classicRenderPass;
        graphicsInfo.subpass = 0;
        vertexInput.vertexBindingDescriptionCount = 0;
        vertexInput.pVertexBindingDescriptions = nullptr;
        vertexInput.vertexAttributeDescriptionCount = 0;
        vertexInput.pVertexAttributeDescriptions = nullptr;
        renderStages[0].module = vertexModule;
        renderStages[1].module = fragmentModule;
        std::array<VkPipelineColorBlendAttachmentState, 2> classicBlendAttachments{{
            blendAttachment, blendAttachment,
        }};
        blendState.attachmentCount =
            static_cast<uint32_t>(classicBlendAttachments.size());
        blendState.pAttachments = classicBlendAttachments.data();
        VkPipeline classicPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &classicPipeline),
              "vkCreateGraphicsPipelines(classic color)");

        VkCommandBuffer prepareClassicRender = beginCommandBuffer(device, commandPool);
        imageBarrier(prepareClassicRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        imageBarrier(prepareClassicRender, classicMrtTarget.image,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        endCommandBuffer(prepareClassicRender);
        std::array<VkClearValue, 2> classicClears{};
        classicClears[0].color = {{1.0f, 0.0f, 0.0f, 1.0f}};
        classicClears[1].color = {{0.0f, 0.0f, 1.0f, 1.0f}};
        VkRenderPassBeginInfo classicBegin =
            makeVkStruct<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        classicBegin.renderPass = classicRenderPass;
        classicBegin.framebuffer = classicFramebuffer;
        classicBegin.renderArea = scissor;
        classicBegin.clearValueCount = static_cast<uint32_t>(classicClears.size());
        classicBegin.pClearValues = classicClears.data();
        VkCommandBuffer classicRender = beginCommandBuffer(device, commandPool);
        vkCmdBeginRenderPass(classicRender, &classicBegin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(classicRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          classicPipeline);
        vkCmdDraw(classicRender, 3, 1, 0, 0);
        vkCmdEndRenderPass(classicRender);
        endCommandBuffer(classicRender);

        VkCommandBuffer finishClassicRender = beginCommandBuffer(device, commandPool);
        imageBarrier(finishClassicRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(finishClassicRender, classicMrtTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishClassicRender);
        VkCommandBuffer readClassicRender = beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readClassicRender, renderTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               renderReadback.buffer, 1, &imageRegion);
        vkCmdCopyImageToBuffer(readClassicRender, classicMrtTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               classicMrtReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readClassicRender);
        std::array<VkSubmitInfo, 4> classicSubmits{};
        for (auto& submit : classicSubmits) {
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        }
        classicSubmits[0].commandBufferCount = 1;
        classicSubmits[0].pCommandBuffers = &prepareClassicRender;
        classicSubmits[1].commandBufferCount = 1;
        classicSubmits[1].pCommandBuffers = &classicRender;
        classicSubmits[2].commandBufferCount = 1;
        classicSubmits[2].pCommandBuffers = &finishClassicRender;
        classicSubmits[3].commandBufferCount = 1;
        classicSubmits[3].pCommandBuffers = &readClassicRender;
        VkFence classicFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(classicSubmits.size()),
                            classicSubmits.data(), classicFence),
              "vkQueueSubmit(classic render sequence)");
        waitFence(device, classicFence);
        validateSolidColor(device, renderReadback, {64, 128, 191, 255});
        validateSolidColor(device, classicMrtReadback, {0, 0, 255, 255});
        std::cout << "CLASSIC_MRT_RENDER_OK" << std::endl;

        // A classic render pass may carry a stencil attachment even when the
        // pipeline cannot read or write stencil. Metal still requires the
        // attachment format and texture to match the render pipeline state.
        Image classicStencilTarget = createImage(
            physicalDevice, device, kImageWidth, kImageHeight,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        std::array<VkAttachmentDescription, 2> classicStencilDescriptions{};
        classicStencilDescriptions[0].format = renderTarget.format;
        classicStencilDescriptions[0].samples = VK_SAMPLE_COUNT_1_BIT;
        classicStencilDescriptions[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        classicStencilDescriptions[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        classicStencilDescriptions[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        classicStencilDescriptions[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        classicStencilDescriptions[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        classicStencilDescriptions[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        classicStencilDescriptions[1].format = classicStencilTarget.format;
        classicStencilDescriptions[1].samples = VK_SAMPLE_COUNT_1_BIT;
        classicStencilDescriptions[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        classicStencilDescriptions[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        classicStencilDescriptions[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        classicStencilDescriptions[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        classicStencilDescriptions[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        classicStencilDescriptions[1].finalLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference classicStencilColorReference{
            0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        VkAttachmentReference classicStencilReference{
            1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        };
        VkSubpassDescription classicStencilSubpass{};
        classicStencilSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        classicStencilSubpass.colorAttachmentCount = 1;
        classicStencilSubpass.pColorAttachments = &classicStencilColorReference;
        classicStencilSubpass.pDepthStencilAttachment = &classicStencilReference;
        VkRenderPassCreateInfo classicStencilRenderPassInfo =
            makeVkStruct<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        classicStencilRenderPassInfo.attachmentCount =
            static_cast<uint32_t>(classicStencilDescriptions.size());
        classicStencilRenderPassInfo.pAttachments = classicStencilDescriptions.data();
        classicStencilRenderPassInfo.subpassCount = 1;
        classicStencilRenderPassInfo.pSubpasses = &classicStencilSubpass;
        VkRenderPass classicStencilRenderPass = VK_NULL_HANDLE;
        check(vkCreateRenderPass(device, &classicStencilRenderPassInfo, nullptr,
                                 &classicStencilRenderPass),
              "vkCreateRenderPass(classic inactive stencil)");

        std::array<VkImageView, 2> classicStencilViews{{
            renderTarget.view, classicStencilTarget.view,
        }};
        VkFramebufferCreateInfo classicStencilFramebufferInfo =
            makeVkStruct<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
        classicStencilFramebufferInfo.renderPass = classicStencilRenderPass;
        classicStencilFramebufferInfo.attachmentCount =
            static_cast<uint32_t>(classicStencilViews.size());
        classicStencilFramebufferInfo.pAttachments = classicStencilViews.data();
        classicStencilFramebufferInfo.width = kImageWidth;
        classicStencilFramebufferInfo.height = kImageHeight;
        classicStencilFramebufferInfo.layers = 1;
        VkFramebuffer classicStencilFramebuffer = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(device, &classicStencilFramebufferInfo, nullptr,
                                  &classicStencilFramebuffer),
              "vkCreateFramebuffer(classic inactive stencil)");

        VkPipelineDepthStencilStateCreateInfo classicInactiveStencilState =
            makeVkStruct<VkPipelineDepthStencilStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        classicInactiveStencilState.stencilTestEnable = VK_FALSE;
        graphicsInfo.renderPass = classicStencilRenderPass;
        graphicsInfo.pDepthStencilState = &classicInactiveStencilState;
        blendState.attachmentCount = 1;
        blendState.pAttachments = &blendAttachment;
        VkPipeline classicInactiveStencilPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &classicInactiveStencilPipeline),
              "vkCreateGraphicsPipelines(classic inactive stencil)");

        VkCommandBuffer prepareClassicStencil = beginCommandBuffer(device, commandPool);
        imageBarrier(prepareClassicStencil, renderTarget.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        endCommandBuffer(prepareClassicStencil);
        std::array<VkClearValue, 2> classicStencilClears{};
        classicStencilClears[0].color = {{1.0f, 0.0f, 0.0f, 1.0f}};
        classicStencilClears[1].depthStencil = {1.0f, 73};
        VkRenderPassBeginInfo classicStencilBegin =
            makeVkStruct<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        classicStencilBegin.renderPass = classicStencilRenderPass;
        classicStencilBegin.framebuffer = classicStencilFramebuffer;
        classicStencilBegin.renderArea = scissor;
        classicStencilBegin.clearValueCount =
            static_cast<uint32_t>(classicStencilClears.size());
        classicStencilBegin.pClearValues = classicStencilClears.data();
        VkCommandBuffer classicStencilRender = beginCommandBuffer(device, commandPool);
        vkCmdBeginRenderPass(classicStencilRender, &classicStencilBegin,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(classicStencilRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          classicInactiveStencilPipeline);
        vkCmdDraw(classicStencilRender, 3, 1, 0, 0);
        vkCmdEndRenderPass(classicStencilRender);
        endCommandBuffer(classicStencilRender);

        VkCommandBuffer finishClassicStencil = beginCommandBuffer(device, commandPool);
        imageBarrier(finishClassicStencil, renderTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishClassicStencil);
        VkCommandBuffer readClassicStencil = beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readClassicStencil, renderTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               renderReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readClassicStencil);
        std::array<VkSubmitInfo, 4> classicStencilSubmits{};
        for (auto& submit : classicStencilSubmits) {
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        }
        classicStencilSubmits[0].commandBufferCount = 1;
        classicStencilSubmits[0].pCommandBuffers = &prepareClassicStencil;
        classicStencilSubmits[1].commandBufferCount = 1;
        classicStencilSubmits[1].pCommandBuffers = &classicStencilRender;
        classicStencilSubmits[2].commandBufferCount = 1;
        classicStencilSubmits[2].pCommandBuffers = &finishClassicStencil;
        classicStencilSubmits[3].commandBufferCount = 1;
        classicStencilSubmits[3].pCommandBuffers = &readClassicStencil;
        VkFence classicStencilFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(classicStencilSubmits.size()),
                            classicStencilSubmits.data(), classicStencilFence),
              "vkQueueSubmit(classic inactive stencil sequence)");
        waitFence(device, classicStencilFence);
        validateSolidColor(device, renderReadback, {64, 128, 191, 255});
        std::cout << "CLASSIC_INACTIVE_STENCIL_RENDER_OK" << std::endl;

        // Static stencil execution must preserve all three Vulkan semantics:
        // an ALWAYS/REPLACE pass writes the reference, EQUAL accepts that
        // value, and the same comparison rejects a different reference.
        VkPipelineDepthStencilStateCreateInfo activeStencilWriteState =
            makeVkStruct<VkPipelineDepthStencilStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        activeStencilWriteState.stencilTestEnable = VK_TRUE;
        activeStencilWriteState.front.failOp = VK_STENCIL_OP_KEEP;
        activeStencilWriteState.front.passOp = VK_STENCIL_OP_REPLACE;
        activeStencilWriteState.front.depthFailOp = VK_STENCIL_OP_KEEP;
        activeStencilWriteState.front.compareOp = VK_COMPARE_OP_ALWAYS;
        activeStencilWriteState.front.compareMask = 0xff;
        activeStencilWriteState.front.writeMask = 0xff;
        activeStencilWriteState.front.reference = 42;
        activeStencilWriteState.back = activeStencilWriteState.front;
        graphicsInfo.pDepthStencilState = &activeStencilWriteState;
        VkPipeline activeStencilWritePipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &activeStencilWritePipeline),
              "vkCreateGraphicsPipelines(static stencil write)");

        VkPipelineDepthStencilStateCreateInfo activeStencilMatchState =
            activeStencilWriteState;
        activeStencilMatchState.front.failOp = VK_STENCIL_OP_KEEP;
        activeStencilMatchState.front.passOp = VK_STENCIL_OP_KEEP;
        activeStencilMatchState.front.depthFailOp = VK_STENCIL_OP_KEEP;
        activeStencilMatchState.front.compareOp = VK_COMPARE_OP_EQUAL;
        activeStencilMatchState.front.writeMask = 0;
        activeStencilMatchState.front.reference = 42;
        activeStencilMatchState.back = activeStencilMatchState.front;
        graphicsInfo.pDepthStencilState = &activeStencilMatchState;
        VkPipeline activeStencilMatchPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &activeStencilMatchPipeline),
              "vkCreateGraphicsPipelines(static stencil match)");

        VkPipelineDepthStencilStateCreateInfo activeStencilMismatchState =
            activeStencilMatchState;
        activeStencilMismatchState.front.reference = 41;
        activeStencilMismatchState.back = activeStencilMismatchState.front;
        graphicsInfo.pDepthStencilState = &activeStencilMismatchState;
        VkPipeline activeStencilMismatchPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &activeStencilMismatchPipeline),
              "vkCreateGraphicsPipelines(static stencil mismatch)");

        std::array<VkAttachmentDescription, 2> activeStencilLoadDescriptions =
            classicStencilDescriptions;
        activeStencilLoadDescriptions[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        activeStencilLoadDescriptions[0].finalLayout =
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        activeStencilLoadDescriptions[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        activeStencilLoadDescriptions[1].initialLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkRenderPassCreateInfo activeStencilLoadRenderPassInfo =
            classicStencilRenderPassInfo;
        activeStencilLoadRenderPassInfo.pAttachments =
            activeStencilLoadDescriptions.data();
        VkRenderPass activeStencilLoadRenderPass = VK_NULL_HANDLE;
        check(vkCreateRenderPass(device, &activeStencilLoadRenderPassInfo, nullptr,
                                 &activeStencilLoadRenderPass),
              "vkCreateRenderPass(static stencil load)");

        Image activeStencilMatchTarget = createImage(
            physicalDevice, device, kImageWidth, kImageHeight);
        Image activeStencilMismatchTarget = createImage(
            physicalDevice, device, kImageWidth, kImageHeight);
        Buffer activeStencilMatchReadback = createBuffer(
            physicalDevice, device, kImageBytes);
        Buffer activeStencilMismatchReadback = createBuffer(
            physicalDevice, device, kImageBytes);
        std::array<VkImageView, 2> activeStencilMatchViews{{
            activeStencilMatchTarget.view, classicStencilTarget.view,
        }};
        VkFramebufferCreateInfo activeStencilMatchFramebufferInfo =
            classicStencilFramebufferInfo;
        activeStencilMatchFramebufferInfo.renderPass = activeStencilLoadRenderPass;
        activeStencilMatchFramebufferInfo.pAttachments =
            activeStencilMatchViews.data();
        VkFramebuffer activeStencilMatchFramebuffer = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(device, &activeStencilMatchFramebufferInfo, nullptr,
                                  &activeStencilMatchFramebuffer),
              "vkCreateFramebuffer(static stencil match)");
        std::array<VkImageView, 2> activeStencilMismatchViews{{
            activeStencilMismatchTarget.view, classicStencilTarget.view,
        }};
        VkFramebufferCreateInfo activeStencilMismatchFramebufferInfo =
            activeStencilMatchFramebufferInfo;
        activeStencilMismatchFramebufferInfo.pAttachments =
            activeStencilMismatchViews.data();
        VkFramebuffer activeStencilMismatchFramebuffer = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(device, &activeStencilMismatchFramebufferInfo, nullptr,
                                  &activeStencilMismatchFramebuffer),
              "vkCreateFramebuffer(static stencil mismatch)");

        VkCommandBuffer activeStencilRender = beginCommandBuffer(device, commandPool);
        imageBarrier(activeStencilRender, renderTarget.image,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        vkCmdBeginRenderPass(activeStencilRender, &classicStencilBegin,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(activeStencilRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          activeStencilWritePipeline);
        vkCmdDraw(activeStencilRender, 3, 1, 0, 0);
        vkCmdEndRenderPass(activeStencilRender);
        VkRenderPassBeginInfo activeStencilMatchBegin = classicStencilBegin;
        activeStencilMatchBegin.renderPass = activeStencilLoadRenderPass;
        activeStencilMatchBegin.framebuffer = activeStencilMatchFramebuffer;
        vkCmdBeginRenderPass(activeStencilRender, &activeStencilMatchBegin,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(activeStencilRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          activeStencilMatchPipeline);
        vkCmdDraw(activeStencilRender, 3, 1, 0, 0);
        vkCmdEndRenderPass(activeStencilRender);
        VkRenderPassBeginInfo activeStencilMismatchBegin = activeStencilMatchBegin;
        activeStencilMismatchBegin.framebuffer = activeStencilMismatchFramebuffer;
        vkCmdBeginRenderPass(activeStencilRender, &activeStencilMismatchBegin,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(activeStencilRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          activeStencilMismatchPipeline);
        vkCmdDraw(activeStencilRender, 3, 1, 0, 0);
        vkCmdEndRenderPass(activeStencilRender);
        endCommandBuffer(activeStencilRender);

        VkCommandBuffer finishActiveStencil = beginCommandBuffer(device, commandPool);
        imageBarrier(finishActiveStencil, activeStencilMatchTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(finishActiveStencil, activeStencilMismatchTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishActiveStencil);
        VkCommandBuffer readActiveStencil = beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readActiveStencil, activeStencilMatchTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               activeStencilMatchReadback.buffer, 1, &imageRegion);
        vkCmdCopyImageToBuffer(readActiveStencil, activeStencilMismatchTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               activeStencilMismatchReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readActiveStencil);
        std::array<VkSubmitInfo, 3> activeStencilSubmits{};
        for (auto& submit : activeStencilSubmits) {
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        }
        activeStencilSubmits[0].commandBufferCount = 1;
        activeStencilSubmits[0].pCommandBuffers = &activeStencilRender;
        activeStencilSubmits[1].commandBufferCount = 1;
        activeStencilSubmits[1].pCommandBuffers = &finishActiveStencil;
        activeStencilSubmits[2].commandBufferCount = 1;
        activeStencilSubmits[2].pCommandBuffers = &readActiveStencil;
        VkFence activeStencilFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(activeStencilSubmits.size()),
                            activeStencilSubmits.data(), activeStencilFence),
              "vkQueueSubmit(static stencil sequence)");
        waitFence(device, activeStencilFence);
        validateSolidColor(device, activeStencilMatchReadback, {64, 128, 191, 255});
        validateSolidColor(device, activeStencilMismatchReadback, {255, 0, 0, 255});
        std::cout << "CLASSIC_STATIC_STENCIL_RENDER_OK" << std::endl;

        // The same static stencil state must work through vkCmdBeginRendering.
        // Reuse the value 42 written above and prove both matching and rejecting
        // dynamic-rendering passes with a combined depth/stencil attachment.
        graphicsInfo.renderPass = VK_NULL_HANDLE;
        graphicsInfo.pNext = &pipelineRendering;
        pipelineRendering.depthAttachmentFormat = classicStencilTarget.format;
        pipelineRendering.stencilAttachmentFormat = classicStencilTarget.format;
        graphicsInfo.pDepthStencilState = &activeStencilMatchState;
        VkPipeline dynamicStencilMatchPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &dynamicStencilMatchPipeline),
              "vkCreateGraphicsPipelines(dynamic stencil match)");
        graphicsInfo.pDepthStencilState = &activeStencilMismatchState;
        VkPipeline dynamicStencilMismatchPipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &dynamicStencilMismatchPipeline),
              "vkCreateGraphicsPipelines(dynamic stencil mismatch)");

        std::array<VkDynamicState, 3> dynamicStencilValueStates{{
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        }};
        VkPipelineDynamicStateCreateInfo dynamicStencilValueState =
            makeVkStruct<VkPipelineDynamicStateCreateInfo>(
                VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamicStencilValueState.dynamicStateCount =
            static_cast<uint32_t>(dynamicStencilValueStates.size());
        dynamicStencilValueState.pDynamicStates = dynamicStencilValueStates.data();
        graphicsInfo.pDepthStencilState = &activeStencilMatchState;
        graphicsInfo.pDynamicState = &dynamicStencilValueState;
        VkPipeline dynamicStencilValuePipeline = VK_NULL_HANDLE;
        check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsInfo,
                                        nullptr, &dynamicStencilValuePipeline),
              "vkCreateGraphicsPipelines(dynamic stencil values)");
        graphicsInfo.pDynamicState = nullptr;

        Image dynamicStencilMatchTarget = createImage(
            physicalDevice, device, kImageWidth, kImageHeight);
        Image dynamicStencilMismatchTarget = createImage(
            physicalDevice, device, kImageWidth, kImageHeight);
        Buffer dynamicStencilMatchReadback = createBuffer(
            physicalDevice, device, kImageBytes);
        Buffer dynamicStencilMismatchReadback = createBuffer(
            physicalDevice, device, kImageBytes);
        VkCommandBuffer prepareDynamicStencil = beginCommandBuffer(device, commandPool);
        imageBarrier(prepareDynamicStencil, dynamicStencilMatchTarget.image,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        imageBarrier(prepareDynamicStencil, dynamicStencilMismatchTarget.image,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        endCommandBuffer(prepareDynamicStencil);

        VkRenderingAttachmentInfo dynamicStencilAttachment =
            makeVkStruct<VkRenderingAttachmentInfo>(
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
        dynamicStencilAttachment.imageView = classicStencilTarget.view;
        dynamicStencilAttachment.imageLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dynamicStencilAttachment.resolveMode = VK_RESOLVE_MODE_NONE;
        dynamicStencilAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        dynamicStencilAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo dynamicStencilColor = colorAttachment;
        dynamicStencilColor.imageView = dynamicStencilMatchTarget.view;
        dynamicStencilColor.clearValue.color = {{1.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo dynamicStencilRendering = renderingInfo;
        dynamicStencilRendering.pColorAttachments = &dynamicStencilColor;
        dynamicStencilRendering.pDepthAttachment = &dynamicStencilAttachment;
        dynamicStencilRendering.pStencilAttachment = &dynamicStencilAttachment;
        VkCommandBuffer dynamicStencilRender = beginCommandBuffer(device, commandPool);
        vkCmdBeginRendering(dynamicStencilRender, &dynamicStencilRendering);
        vkCmdBindPipeline(dynamicStencilRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dynamicStencilMatchPipeline);
        vkCmdDraw(dynamicStencilRender, 3, 1, 0, 0);
        vkCmdEndRendering(dynamicStencilRender);
        dynamicStencilColor.imageView = dynamicStencilMismatchTarget.view;
        vkCmdBeginRendering(dynamicStencilRender, &dynamicStencilRendering);
        vkCmdBindPipeline(dynamicStencilRender, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dynamicStencilMismatchPipeline);
        vkCmdDraw(dynamicStencilRender, 3, 1, 0, 0);
        vkCmdEndRendering(dynamicStencilRender);
        endCommandBuffer(dynamicStencilRender);

        VkCommandBuffer finishDynamicStencil = beginCommandBuffer(device, commandPool);
        imageBarrier(finishDynamicStencil, dynamicStencilMatchTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(finishDynamicStencil, dynamicStencilMismatchTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishDynamicStencil);
        VkCommandBuffer readDynamicStencil = beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readDynamicStencil, dynamicStencilMatchTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dynamicStencilMatchReadback.buffer, 1, &imageRegion);
        vkCmdCopyImageToBuffer(readDynamicStencil, dynamicStencilMismatchTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dynamicStencilMismatchReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readDynamicStencil);
        std::array<VkSubmitInfo, 4> dynamicStencilSubmits{};
        for (auto& submit : dynamicStencilSubmits) {
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        }
        dynamicStencilSubmits[0].commandBufferCount = 1;
        dynamicStencilSubmits[0].pCommandBuffers = &prepareDynamicStencil;
        dynamicStencilSubmits[1].commandBufferCount = 1;
        dynamicStencilSubmits[1].pCommandBuffers = &dynamicStencilRender;
        dynamicStencilSubmits[2].commandBufferCount = 1;
        dynamicStencilSubmits[2].pCommandBuffers = &finishDynamicStencil;
        dynamicStencilSubmits[3].commandBufferCount = 1;
        dynamicStencilSubmits[3].pCommandBuffers = &readDynamicStencil;
        VkFence dynamicStencilFence = createFence(device);
        check(vkQueueSubmit(queue, static_cast<uint32_t>(dynamicStencilSubmits.size()),
                            dynamicStencilSubmits.data(), dynamicStencilFence),
              "vkQueueSubmit(dynamic stencil sequence)");
        waitFence(device, dynamicStencilFence);
        validateSolidColor(device, dynamicStencilMatchReadback, {64, 128, 191, 255});
        validateSolidColor(device, dynamicStencilMismatchReadback, {255, 0, 0, 255});
        std::cout << "DYNAMIC_STATIC_STENCIL_RENDER_OK" << std::endl;

        Image dynamicStencilValueMatchTarget = createImage(
            physicalDevice, device, kImageWidth, kImageHeight);
        Image dynamicStencilValueMismatchTarget = createImage(
            physicalDevice, device, kImageWidth, kImageHeight);
        Buffer dynamicStencilValueMatchReadback = createBuffer(
            physicalDevice, device, kImageBytes);
        Buffer dynamicStencilValueMismatchReadback = createBuffer(
            physicalDevice, device, kImageBytes);
        VkCommandBuffer prepareDynamicStencilValues =
            beginCommandBuffer(device, commandPool);
        imageBarrier(prepareDynamicStencilValues, dynamicStencilValueMatchTarget.image,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        imageBarrier(prepareDynamicStencilValues,
                     dynamicStencilValueMismatchTarget.image,
                     VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        endCommandBuffer(prepareDynamicStencilValues);

        VkCommandBuffer renderDynamicStencilValues =
            beginCommandBuffer(device, commandPool);
        dynamicStencilColor.imageView = dynamicStencilValueMatchTarget.view;
        vkCmdBeginRendering(renderDynamicStencilValues, &dynamicStencilRendering);
        vkCmdBindPipeline(renderDynamicStencilValues, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dynamicStencilValuePipeline);
        vkCmdSetStencilCompareMask(renderDynamicStencilValues,
                                   VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
        vkCmdSetStencilWriteMask(renderDynamicStencilValues,
                                 VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        vkCmdSetStencilReference(renderDynamicStencilValues,
                                 VK_STENCIL_FACE_FRONT_AND_BACK, 42);
        vkCmdDraw(renderDynamicStencilValues, 3, 1, 0, 0);
        vkCmdEndRendering(renderDynamicStencilValues);
        dynamicStencilColor.imageView = dynamicStencilValueMismatchTarget.view;
        vkCmdBeginRendering(renderDynamicStencilValues, &dynamicStencilRendering);
        vkCmdBindPipeline(renderDynamicStencilValues, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          dynamicStencilValuePipeline);
        vkCmdSetStencilCompareMask(renderDynamicStencilValues,
                                   VK_STENCIL_FACE_FRONT_AND_BACK, 0xff);
        vkCmdSetStencilWriteMask(renderDynamicStencilValues,
                                 VK_STENCIL_FACE_FRONT_AND_BACK, 0);
        vkCmdSetStencilReference(renderDynamicStencilValues,
                                 VK_STENCIL_FACE_FRONT_AND_BACK, 41);
        vkCmdDraw(renderDynamicStencilValues, 3, 1, 0, 0);
        vkCmdEndRendering(renderDynamicStencilValues);
        endCommandBuffer(renderDynamicStencilValues);

        VkCommandBuffer finishDynamicStencilValues =
            beginCommandBuffer(device, commandPool);
        imageBarrier(finishDynamicStencilValues, dynamicStencilValueMatchTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(finishDynamicStencilValues,
                     dynamicStencilValueMismatchTarget.image,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        endCommandBuffer(finishDynamicStencilValues);
        VkCommandBuffer readDynamicStencilValues =
            beginCommandBuffer(device, commandPool);
        vkCmdCopyImageToBuffer(readDynamicStencilValues,
                               dynamicStencilValueMatchTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dynamicStencilValueMatchReadback.buffer, 1, &imageRegion);
        vkCmdCopyImageToBuffer(readDynamicStencilValues,
                               dynamicStencilValueMismatchTarget.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               dynamicStencilValueMismatchReadback.buffer, 1, &imageRegion);
        endCommandBuffer(readDynamicStencilValues);
        std::array<VkSubmitInfo, 4> dynamicStencilValueSubmits{};
        for (auto& submit : dynamicStencilValueSubmits) {
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        }
        dynamicStencilValueSubmits[0].commandBufferCount = 1;
        dynamicStencilValueSubmits[0].pCommandBuffers = &prepareDynamicStencilValues;
        dynamicStencilValueSubmits[1].commandBufferCount = 1;
        dynamicStencilValueSubmits[1].pCommandBuffers = &renderDynamicStencilValues;
        dynamicStencilValueSubmits[2].commandBufferCount = 1;
        dynamicStencilValueSubmits[2].pCommandBuffers = &finishDynamicStencilValues;
        dynamicStencilValueSubmits[3].commandBufferCount = 1;
        dynamicStencilValueSubmits[3].pCommandBuffers = &readDynamicStencilValues;
        VkFence dynamicStencilValueFence = createFence(device);
        check(vkQueueSubmit(queue,
                            static_cast<uint32_t>(dynamicStencilValueSubmits.size()),
                            dynamicStencilValueSubmits.data(), dynamicStencilValueFence),
              "vkQueueSubmit(dynamic stencil values sequence)");
        waitFence(device, dynamicStencilValueFence);
        validateSolidColor(device, dynamicStencilValueMatchReadback,
                           {64, 128, 191, 255});
        validateSolidColor(device, dynamicStencilValueMismatchReadback,
                           {255, 0, 0, 255});
        std::cout << "DYNAMIC_STENCIL_VALUES_OK" << std::endl;

        // An isolated query reset must stay on the MTL4 backend and publish the
        // unavailable state only after the reset command buffer commits.
        VkQueryPoolCreateInfo queryPoolInfo = makeVkStruct<VkQueryPoolCreateInfo>(
            VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO);
        queryPoolInfo.queryType = VK_QUERY_TYPE_OCCLUSION;
        queryPoolInfo.queryCount = 4;
        VkQueryPool queryPool = VK_NULL_HANDLE;
        check(vkCreateQueryPool(device, &queryPoolInfo, nullptr, &queryPool),
              "vkCreateQueryPool(occlusion)");
        VkCommandBuffer resetQueries = beginCommandBuffer(device, commandPool);
        vkCmdResetQueryPool(resetQueries, queryPool, 1, 2);
        endCommandBuffer(resetQueries);
        VkSubmitInfo queryResetSubmit = makeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        queryResetSubmit.commandBufferCount = 1;
        queryResetSubmit.pCommandBuffers = &resetQueries;
        VkFence queryResetFence = createFence(device);
        check(vkQueueSubmit(queue, 1, &queryResetSubmit, queryResetFence),
              "vkQueueSubmit(query reset)");
        waitFence(device, queryResetFence);
        std::array<uint64_t, 4> queryResults{{UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX}};
        VkResult queryResult = vkGetQueryPoolResults(
            device, queryPool, 1, 2, sizeof(queryResults), queryResults.data(),
            sizeof(uint64_t) * 2,
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_PARTIAL_BIT |
                VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        check(queryResult, "vkGetQueryPoolResults(after reset)");
        if (queryResults[0] != 0 || queryResults[1] != 0 ||
            queryResults[2] != 0 || queryResults[3] != 0) {
            fail("Query reset did not clear result bytes and availability");
        }
        std::cout << "QUERY_RESET_OK" << std::endl;

        VkCommandBuffer queryCommand = beginCommandBuffer(device, commandPool);
        vkCmdBeginRendering(queryCommand, &renderingInfo);
        vkCmdBindPipeline(queryCommand, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
        vkCmdBeginQuery(queryCommand, queryPool, 1, 0);
        vkCmdDraw(queryCommand, 3, 1, 0, 0);
        vkCmdEndQuery(queryCommand, queryPool, 1);
        vkCmdEndRendering(queryCommand);
        endCommandBuffer(queryCommand);
        VkSubmitInfo querySubmit = makeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        querySubmit.commandBufferCount = 1;
        querySubmit.pCommandBuffers = &queryCommand;
        VkFence queryFence = createFence(device);
        check(vkQueueSubmit(queue, 1, &querySubmit, queryFence),
              "vkQueueSubmit(occlusion query)");
        waitFence(device, queryFence);
        std::array<uint64_t, 2> occlusionResult{{0, 0}};
        check(vkGetQueryPoolResults(
                  device, queryPool, 1, 1, sizeof(occlusionResult), occlusionResult.data(),
                  sizeof(occlusionResult), VK_QUERY_RESULT_64_BIT |
                      VK_QUERY_RESULT_WAIT_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT),
              "vkGetQueryPoolResults(occlusion)");
        if (occlusionResult[0] == 0 || occlusionResult[1] != 1) {
            fail("Occlusion query result or availability was not published");
        }
        std::cout << "QUERY_OCCLUSION_OK" << std::endl;

        // vkCmdUpdateBuffer owns its source bytes in the recorded command. The
        // MTL4 preflight must create resident staging storage before commit.
        Buffer updated = createBuffer(physicalDevice, device, 256);
        std::array<uint32_t, 64> updateWords{};
        std::vector<uint8_t> expectedUpdate(sizeof(updateWords));
        for (size_t index = 0; index < updateWords.size(); ++index) {
            updateWords[index] = static_cast<uint32_t>(0x10203040u + index * 0x01010101u);
        }
        std::memcpy(expectedUpdate.data(), updateWords.data(), expectedUpdate.size());
        VkCommandBuffer updateCommand = beginCommandBuffer(device, commandPool);
        vkCmdUpdateBuffer(updateCommand, updated.buffer, 0, sizeof(updateWords), updateWords.data());
        endCommandBuffer(updateCommand);
        VkSubmitInfo updateSubmit = makeVkStruct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        updateSubmit.commandBufferCount = 1;
        updateSubmit.pCommandBuffers = &updateCommand;
        VkFence updateFence = createFence(device);
        check(vkQueueSubmit(queue, 1, &updateSubmit, updateFence),
              "vkQueueSubmit(update buffer)");
        waitFence(device, updateFence);
        validateBytes(device, updated, expectedUpdate);
        std::cout << "UPDATE_BUFFER_OK" << std::endl;

        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

        vkDestroyFence(device, classicFence, nullptr);
        vkDestroyFence(device, classicStencilFence, nullptr);
        vkDestroyFence(device, activeStencilFence, nullptr);
        vkDestroyFence(device, dynamicStencilFence, nullptr);
        vkDestroyFence(device, dynamicStencilValueFence, nullptr);
        vkDestroyPipeline(device, dynamicStencilMatchPipeline, nullptr);
        vkDestroyPipeline(device, dynamicStencilMismatchPipeline, nullptr);
        vkDestroyPipeline(device, dynamicStencilValuePipeline, nullptr);
        dynamicStencilMatchTarget.destroy();
        dynamicStencilMismatchTarget.destroy();
        dynamicStencilMatchReadback.destroy();
        dynamicStencilMismatchReadback.destroy();
        dynamicStencilValueMatchTarget.destroy();
        dynamicStencilValueMismatchTarget.destroy();
        dynamicStencilValueMatchReadback.destroy();
        dynamicStencilValueMismatchReadback.destroy();
        vkDestroyPipeline(device, activeStencilWritePipeline, nullptr);
        vkDestroyPipeline(device, activeStencilMatchPipeline, nullptr);
        vkDestroyPipeline(device, activeStencilMismatchPipeline, nullptr);
        vkDestroyFramebuffer(device, activeStencilMatchFramebuffer, nullptr);
        vkDestroyFramebuffer(device, activeStencilMismatchFramebuffer, nullptr);
        vkDestroyRenderPass(device, activeStencilLoadRenderPass, nullptr);
        activeStencilMatchTarget.destroy();
        activeStencilMismatchTarget.destroy();
        activeStencilMatchReadback.destroy();
        activeStencilMismatchReadback.destroy();
        vkDestroyPipeline(device, classicInactiveStencilPipeline, nullptr);
        vkDestroyFramebuffer(device, classicStencilFramebuffer, nullptr);
        vkDestroyRenderPass(device, classicStencilRenderPass, nullptr);
        classicStencilTarget.destroy();
        vkDestroyPipeline(device, classicPipeline, nullptr);
        vkDestroyFramebuffer(device, classicFramebuffer, nullptr);
        vkDestroyRenderPass(device, classicRenderPass, nullptr);
        classicMrtTarget.destroy();
        classicMrtReadback.destroy();
        vkDestroyFence(device, depthFence, nullptr);
        vkDestroyPipeline(device, depthPipeline, nullptr);
        depthTarget.destroy();
        vkDestroyPipeline(device, dynamicViewportScissorPipeline, nullptr);
        vkDestroyPipeline(device, multiViewportScissorPipeline, nullptr);
        vkDestroyPipeline(device, activeBlendConstantsPipeline, nullptr);
        vkDestroyPipeline(device, inactiveStencilDynamicPipeline, nullptr);
        vkDestroyPipeline(device, inactiveDepthBiasDynamicPipeline, nullptr);
        vkDestroyFence(device, dynamicVertexFence, nullptr);
        vkDestroyPipeline(device, dynamicVertexPipeline, nullptr);
        vkDestroyFence(device, vertexFence, nullptr);
        vkDestroyPipeline(device, vertexInputPipeline, nullptr);
        vkDestroyShaderModule(device, vertexInputModule, nullptr);
        vertexBuffer.destroy();
        vkDestroyFence(device, descriptorFence, nullptr);
        vkDestroyPipeline(device, descriptorPipeline, nullptr);
        vkDestroyShaderModule(device, descriptorFragmentModule, nullptr);
        vkDestroyPipelineLayout(device, descriptorPipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        uniformColor.destroy();
        vkDestroyFence(device, updateFence, nullptr);
        updated.destroy();
        vkDestroyFence(device, queryFence, nullptr);
        vkDestroyFence(device, queryResetFence, nullptr);
        vkDestroyQueryPool(device, queryPool, nullptr);
        vkDestroyFence(device, renderFence, nullptr);
        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        vkDestroyPipelineLayout(device, renderLayout, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
        vkDestroyShaderModule(device, vertexModule, nullptr);
        renderTarget.destroy();
        renderReadback.destroy();
        vkDestroyFence(device, imageFence, nullptr);
        vkDestroyFence(device, computeFence, nullptr);
        vkDestroyPipeline(device, computePipeline, nullptr);
        vkDestroyPipelineLayout(device, computeLayout, nullptr);
        vkDestroyShaderModule(device, computeModule, nullptr);
        vkDestroyFence(device, timelineFence, nullptr);
        vkDestroySemaphore(device, timelineSemaphore, nullptr);
        vkDestroyFence(device, semaphoreFence, nullptr);
        vkDestroySemaphore(device, semaphore, nullptr);
        srcImage.destroy();
        dstImage.destroy();
        imageUpload.destroy();
        imageReadback.destroy();
        a.destroy();
        b.destroy();
        c.destroy();
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);

        std::cout << "METAL4_PHASE1C_E2E_PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "METAL4_TRANSFER_E2E_FAIL: " << error.what() << std::endl;
        return 1;
    }
}
