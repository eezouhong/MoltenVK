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
    createInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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
                  uint32_t height) {
    Image result;
    result.device = device;
    result.format = VK_FORMAT_R8G8B8A8_UNORM;
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
    createInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
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
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
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

void imageBarrier(VkCommandBuffer commandBuffer,
                  VkImage image,
                  VkImageLayout oldLayout,
                  VkImageLayout newLayout,
                  VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess,
                  VkPipelineStageFlags srcStage,
                  VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier = makeVkStruct<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
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

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo = makeVkStruct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &priority;

        VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeatures = makeVkStruct<VkPhysicalDeviceDynamicRenderingFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES);
        VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures = makeVkStruct<VkPhysicalDeviceTimelineSemaphoreFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES);
        timelineFeatures.pNext = &dynamicRenderingFeatures;
        VkPhysicalDeviceFeatures2 supportedFeatures = makeVkStruct<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        supportedFeatures.pNext = &timelineFeatures;
        vkGetPhysicalDeviceFeatures2(physicalDevice, &supportedFeatures);
        if (!timelineFeatures.timelineSemaphore) { fail("Timeline semaphores are unavailable"); }
        if (!dynamicRenderingFeatures.dynamicRendering) { fail("Dynamic rendering is unavailable"); }
        timelineFeatures.timelineSemaphore = VK_TRUE;
        dynamicRenderingFeatures.dynamicRendering = VK_TRUE;

        VkDeviceCreateInfo deviceCreateInfo = makeVkStruct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        deviceCreateInfo.pNext = &timelineFeatures;
        deviceCreateInfo.queueCreateInfoCount = 1;
        deviceCreateInfo.pQueueCreateInfos = &queueCreateInfo;
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

        vkDestroyFence(device, updateFence, nullptr);
        updated.destroy();
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
