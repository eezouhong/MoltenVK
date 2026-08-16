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
            *coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
            return index;
        }
        if (fallback == UINT32_MAX) { fallback = index; }
    }
    if (fallback == UINT32_MAX) { fail("No host-visible Vulkan memory type"); }
    *coherent = (properties.memoryTypes[fallback].propertyFlags &
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return fallback;
}

Buffer createBuffer(VkPhysicalDevice physicalDevice, VkDevice device, VkDeviceSize size) {
    Buffer result;
    result.device = device;
    result.size = size;

    VkBufferCreateInfo createInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
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

    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = memoryType;
    check(vkAllocateMemory(device, &allocateInfo, nullptr, &result.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(device, result.buffer, result.memory, 0), "vkBindBufferMemory");
    result.coherent = coherent;
    return result;
}

VkCommandBuffer beginCommandBuffer(VkDevice device, VkCommandPool commandPool) {
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = 1;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(device, &allocateInfo, &commandBuffer), "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(commandBuffer, &beginInfo), "vkBeginCommandBuffer");
    return commandBuffer;
}

void endCommandBuffer(VkCommandBuffer commandBuffer) {
    check(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
}

VkFence createFence(VkDevice device) {
    VkFenceCreateInfo createInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
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
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
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

        VkApplicationInfo applicationInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        applicationInfo.pApplicationName = "MoltenVK Metal 4 transfer e2e";
        applicationInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instanceCreateInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
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
            if ((queueFamilies[index].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0) {
                queueFamilyIndex = index;
                break;
            }
        }
        if (queueFamilyIndex == UINT32_MAX) { fail("No Vulkan transfer queue"); }

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

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueCreateInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo deviceCreateInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
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

        VkCommandPoolCreateInfo poolCreateInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
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
        VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
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

        VkSemaphoreCreateInfo semaphoreCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore semaphore = VK_NULL_HANDLE;
        check(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &semaphore),
              "vkCreateSemaphore");
        VkSubmitInfo signalSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        signalSubmit.commandBufferCount = 1;
        signalSubmit.pCommandBuffers = &fillWithSemaphore;
        signalSubmit.signalSemaphoreCount = 1;
        signalSubmit.pSignalSemaphores = &semaphore;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo waitSubmit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
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

        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");

        vkDestroyFence(device, semaphoreFence, nullptr);
        vkDestroySemaphore(device, semaphore, nullptr);
        vkFreeCommandBuffers(device, commandPool, 1, &fillA);
        vkFreeCommandBuffers(device, commandPool, 1, &fallbackCopy);
        vkFreeCommandBuffers(device, commandPool, 1, &copyToReadback);
        vkFreeCommandBuffers(device, commandPool, 1, &fillWithSemaphore);
        vkFreeCommandBuffers(device, commandPool, 1, &copyWithSemaphore);
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);

        std::cout << "METAL4_TRANSFER_E2E_PASS" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "METAL4_TRANSFER_E2E_FAIL: " << error.what() << std::endl;
        return 1;
    }
}
