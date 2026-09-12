// SPDX-License-Identifier: Apache-2.0
// Real Vulkan/Metal integration: concurrent creation, cache roundtrip and GPU readback.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>
using namespace std;
static void check(VkResult r, const char *what) {
    if (r != VK_SUCCESS)
        throw runtime_error(string(what) + ": " + to_string(r));
}
static void require(bool ok, const char *what) {
    if (!ok)
        throw runtime_error(what);
}
static vector<uint32_t> readSpv(const char *path) {
    ifstream f(path, ios::binary | ios::ate);
    require(bool(f), "shader file missing");
    auto size = f.tellg();
    require(size > 0 && size % 4 == 0, "invalid SPIR-V length");
    vector<uint32_t> v(size / 4);
    f.seekg(0);
    f.read((char *)v.data(), size);
    return v;
}
struct Context {
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family = 0;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDescriptorPool descriptorPool{};
    VkCommandPool commandPool{};
    vector<VkShaderModule> modules;
    vector<VkPipelineCache> caches;
    vector<VkPipeline> pipelines;
    bool cacheControl = false;
    bool creationFeedback = false;
    void *mapped = nullptr;
    ~Context() {
        if (device) {
            vkDeviceWaitIdle(device);
            if (mapped)
                vkUnmapMemory(device, memory);
            for (auto p : pipelines)
                vkDestroyPipeline(device, p, nullptr);
            for (auto c : caches)
                if (c)
                    vkDestroyPipelineCache(device, c, nullptr);
            for (auto m : modules)
                vkDestroyShaderModule(device, m, nullptr);
            if (commandPool)
                vkDestroyCommandPool(device, commandPool, nullptr);
            if (descriptorPool)
                vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            if (buffer)
                vkDestroyBuffer(device, buffer, nullptr);
            if (memory)
                vkFreeMemory(device, memory, nullptr);
            if (layout)
                vkDestroyPipelineLayout(device, layout, nullptr);
            if (setLayout)
                vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
    void init() {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "rc6-library-concurrency";
        app.apiVersion = VK_API_VERSION_1_1;
        uint32_t extensionCount = 0;
        check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr),
              "instance extensions");
        vector<VkExtensionProperties> instanceExtensions(extensionCount);
        check(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
                                                     instanceExtensions.data()),
              "instance extensions");
        bool portability = false;
        for (auto &e : instanceExtensions)
            portability |= !strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        const char *exts[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        if (portability) {
            ci.enabledExtensionCount = 1;
            ci.ppEnabledExtensionNames = exts;
            ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
        check(vkCreateInstance(&ci, nullptr, &instance), "instance");
        uint32_t n = 0;
        check(vkEnumeratePhysicalDevices(instance, &n, nullptr), "physical count");
        require(n > 0, "no GPU");
        vector<VkPhysicalDevice> devices(n);
        check(vkEnumeratePhysicalDevices(instance, &n, devices.data()), "physical");
        physical = devices[0];
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
        vector<VkQueueFamilyProperties> families(n);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, families.data());
        for (family = 0; family < n; ++family)
            if (families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)
                break;
        require(family < n, "no compute queue");
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr),
              "extension count");
        vector<VkExtensionProperties> available(n);
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, available.data()),
              "extensions");
        vector<const char *> enabled;
        for (auto &e : available) {
            if (!strcmp(e.extensionName, "VK_KHR_portability_subset"))
                enabled.push_back("VK_KHR_portability_subset");
            if (!strcmp(e.extensionName, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME)) {
                enabled.push_back(VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME);
                cacheControl = true;
            }
            if (!strcmp(e.extensionName, VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME)) {
                enabled.push_back(VK_EXT_PIPELINE_CREATION_FEEDBACK_EXTENSION_NAME);
                creationFeedback = true;
            }
        }
        VkPhysicalDevicePipelineCreationCacheControlFeatures feat{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (cacheControl) {
            features.pNext = &feat;
            vkGetPhysicalDeviceFeatures2(physical, &features);
            cacheControl = feat.pipelineCreationCacheControl;
        }
        float priority = 1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dc.queueCreateInfoCount = 1;
        dc.pQueueCreateInfos = &qci;
        dc.enabledExtensionCount = enabled.size();
        dc.ppEnabledExtensionNames = enabled.data();
        if (cacheControl)
            dc.pNext = &feat;
        check(vkCreateDevice(physical, &dc, nullptr, &device), "device");
        vkGetDeviceQueue(device, family, 0, &queue);
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 1;
        sl.pBindings = &b;
        check(vkCreateDescriptorSetLayout(device, &sl, nullptr, &setLayout), "set layout");
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &pl, nullptr, &layout), "layout");
    }
    VkPipelineCache cache(const vector<uint8_t> &bytes = {}) {
        VkPipelineCacheCreateInfo c{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        c.initialDataSize = bytes.size();
        c.pInitialData = bytes.empty() ? nullptr : bytes.data();
        VkPipelineCache result;
        check(vkCreatePipelineCache(device, &c, nullptr, &result), "cache");
        caches.push_back(result);
        return result;
    }
    VkShaderModule module(const char *path) {
        auto spv = readSpv(path);
        VkShaderModuleCreateInfo m{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        m.codeSize = spv.size() * 4;
        m.pCode = spv.data();
        VkShaderModule result;
        check(vkCreateShaderModule(device, &m, nullptr, &result), "module");
        modules.push_back(result);
        return result;
    }
    VkResult create(VkShaderModule module, VkPipelineCache cache, uint32_t seed, VkPipeline *result,
                    VkPipelineCreateFlags flags = 0,
                    VkPipelineCreationFeedback *stageFeedback = nullptr) {
        VkSpecializationMapEntry e{0, 0, 4};
        VkSpecializationInfo spec{1, &e, 4, &seed};
        VkComputePipelineCreateInfo p{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        VkPipelineCreationFeedback overall{};
        VkPipelineCreationFeedbackCreateInfo feedback{VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO};
        if (stageFeedback) {
            feedback.pPipelineCreationFeedback = &overall;
            feedback.pipelineStageCreationFeedbackCount = 1;
            feedback.pPipelineStageCreationFeedbacks = stageFeedback;
            p.pNext = &feedback;
        }
        p.flags = flags;
        p.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        p.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        p.stage.module = module;
        p.stage.pName = "main";
        p.stage.pSpecializationInfo = &spec;
        p.layout = layout;
        return vkCreateComputePipelines(device, cache, 1, &p, nullptr, result);
    }
    vector<uint8_t> exportCache(VkPipelineCache cache) {
        for (int attempt = 0; attempt < 20; ++attempt) {
            size_t size = 0;
            check(vkGetPipelineCacheData(device, cache, &size, nullptr), "cache size");
            vector<uint8_t> bytes(size);
            VkResult r = vkGetPipelineCacheData(device, cache, &size, bytes.data());
            if (r == VK_INCOMPLETE)
                continue;
            check(r, "export");
            require(size >= 32 && size <= bytes.size(), "invalid successful cache export length");
            bytes.resize(size);
            uint32_t header, version;
            memcpy(&header, bytes.data(), 4);
            memcpy(&version, bytes.data() + 4, 4);
            require(header >= 32 && header <= size && version == 1, "invalid cache header");
            return bytes;
        }
        throw runtime_error("cache export retries exhausted");
    }
    void execute(const vector<uint32_t> &expected) {
        VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = expected.size() * 4;
        bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        check(vkCreateBuffer(device, &bc, nullptr, &buffer), "buffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(device, buffer, &req);
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(physical, &mp);
        uint32_t type = 0;
        for (; type < mp.memoryTypeCount; ++type)
            if ((req.memoryTypeBits & (1u << type)) &&
                (mp.memoryTypes[type].propertyFlags &
                 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                    (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
                break;
        require(type < mp.memoryTypeCount, "no coherent host memory");
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = req.size;
        ma.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &ma, nullptr, &memory), "memory");
        check(vkBindBufferMemory(device, buffer, memory, 0), "bind");
        check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map");
        memset(mapped, 0, expected.size() * 4);
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dp.maxSets = 1;
        dp.poolSizeCount = 1;
        dp.pPoolSizes = &ps;
        check(vkCreateDescriptorPool(device, &dp, nullptr, &descriptorPool), "descriptor pool");
        VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sa.descriptorPool = descriptorPool;
        sa.descriptorSetCount = 1;
        sa.pSetLayouts = &setLayout;
        VkDescriptorSet set;
        check(vkAllocateDescriptorSets(device, &sa, &set), "set");
        VkDescriptorBufferInfo bi{buffer, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &pc, nullptr, &commandPool), "command pool");
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = commandPool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        VkCommandBuffer cmd;
        check(vkAllocateCommandBuffers(device, &ca, &cmd), "commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &begin), "begin");
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0,
                                nullptr);
        for (uint32_t i = 0; i < pipelines.size(); ++i) {
            uint32_t offset = i * 4;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[i]);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &offset);
            vkCmdDispatch(cmd, 4, 1, 1);
        }
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(cmd), "end");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "submit");
        check(vkQueueWaitIdle(queue), "wait");
        for (size_t i = 0; i < expected.size(); ++i)
            require(((uint32_t *)mapped)[i] == expected[i], "GPU readback mismatch");
    }
};
static int testCachelessReuse(Context &c, char **argv, bool repositoryEnabled) {
    require(c.cacheControl, "cache control required for cacheless reuse test");
    require(c.creationFeedback, "creation feedback required for cacheless reuse test");
    VkPipeline absent{};
    require(c.create(c.modules[3], VK_NULL_HANDLE, 17, &absent,
                VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT) == VK_PIPELINE_COMPILE_REQUIRED,
            "fresh cacheless module must not compile when compilation is prohibited");
    require(absent == VK_NULL_HANDLE, "compile-required returned a pipeline");
    cout << "FreshCachelessCompileRequired PASS\n";
    vector<uint32_t> expected;
    bool sharingCorrect = true;
    auto keep = [&](VkPipeline p, unsigned moduleId) {
        c.pipelines.push_back(p);
        for (unsigned j = 0; j < 4; ++j)
            expected.push_back(moduleId * 10000 + 17 + j);
    };
    auto attemptReuse = [&](VkShaderModule module, VkPipelineCache cache, unsigned id,
                            const char *label) {
        VkPipeline pipeline{};
        VkPipelineCreationFeedback feedback{};
        VkResult result = c.create(module, cache, 17, &pipeline,
            VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, &feedback);
        bool reused = result == VK_SUCCESS;
        require(reused || result == VK_PIPELINE_COMPILE_REQUIRED, "unexpected reuse failure");
        if (cache == VK_NULL_HANDLE)
            require(!(feedback.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT),
                    "cacheless creation incorrectly reports an application cache hit");
        sharingCorrect &= reused == repositoryEnabled;
        cout << label << " reused=" << reused << " expected=" << repositoryEnabled << '\n';
        if (!reused)
            check(c.create(module, cache, 17, &pipeline), "fallback creation after reuse probe");
        keep(pipeline, id);
    };

    auto shared = c.cache();
    VkPipeline first{};
    check(c.create(c.modules[0], shared, 17, &first), "cached seed");
    keep(first, 0);
    auto before = c.exportCache(shared);
    // A different VkShaderModule object with identical bytes must reuse the
    // existing device library even when the caller cannot acquire its Vk cache.
    attemptReuse(c.module(argv[1]), VK_NULL_HANDLE, 0, "CachedToCacheless");
    require(c.exportCache(shared) == before, "cacheless reuse changed source cache membership");

    VkPipeline uncached{};
    check(c.create(c.modules[1], VK_NULL_HANDLE, 17, &uncached), "cacheless seed");
    keep(uncached, 1);
    attemptReuse(c.module(argv[2]), c.cache(), 1, "CachelessToCached");

    // Different native modules containing the same new SPIR-V contend for the
    // same device library. No VkPipelineCache is present in any of these calls.
    VkShaderModule duplicate = c.module(argv[3]);
    vector<future<VkPipeline>> tasks;
    atomic<bool> go{false};
    for (int i = 0; i < 8; ++i)
        tasks.push_back(async(launch::async, [&, i] {
            while (!go.load()) this_thread::yield();
            VkPipeline p{};
            check(c.create(i % 2 ? duplicate : c.modules[2], VK_NULL_HANDLE, 17, &p),
                  "concurrent cacheless create");
            return p;
        }));
    go = true;
    for (auto &t : tasks) keep(t.get(), 2);
    cout << "ConcurrentCachelessCreation PASS\n";

    tasks.clear();
    go = false;
    VkPipelineCache mixedCache = c.cache();
    for (int i = 0; i < 8; ++i)
        tasks.push_back(async(launch::async, [&, i] {
            while (!go.load()) this_thread::yield();
            VkPipeline p{};
            check(c.create(c.modules[3], i % 2 ? mixedCache : VK_NULL_HANDLE, 17, &p),
                  "mixed cached/cacheless create");
            return p;
        }));
    go = true;
    for (auto &t : tasks) keep(t.get(), 3);
    cout << "ConcurrentMixedCacheCreation PASS\n";

    // Exercise the reverse ownership direction: original logical Vk cache and
    // all source modules can die before pipelines execute on the GPU.
    for (auto cache : c.caches) vkDestroyPipelineCache(c.device, cache, nullptr);
    c.caches.clear();
    for (auto module : c.modules) vkDestroyShaderModule(c.device, module, nullptr);
    c.modules.clear();
    c.execute(expected);
    cout << "CachelessGpuReadbackAfterOwnersDestroyed PASS words=" << expected.size() << '\n';
    cout << "ExpectedLibraryCompiles=" << (repositoryEnabled ? 4 : 8) << '\n';
    require(sharingCorrect, "cacheless path did not share existing device library");
    return 0;
}
int main(int argc, char **argv) {
    try {
        require(argc == 5 || argc == 6, "expected four SPIR-V modules and optional cacheless mode");
        Context c;
        c.init();
        for (int i = 1; i < 5; ++i)
            c.module(argv[i]);
        if (argc == 6)
            return testCachelessReuse(c, argv, string(argv[5]) == "cacheless-repository-on");
        auto shared = c.cache();
        auto other = c.cache();
        if (c.cacheControl) {
            VkPipeline absent{};
            auto r = c.create(c.modules[0], shared, 17, &absent,
                              VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
            if (absent)
                vkDestroyPipeline(c.device, absent, nullptr);
            require(r == VK_PIPELINE_COMPILE_REQUIRED,
                    "fresh cache did not return compile-required");
            cout << "FreshCompileRequired PASS\n";
        }
        vector<future<VkPipeline>> tasks;
        atomic<bool> go{false};
        for (int i = 0; i < 16; ++i)
            tasks.push_back(async(launch::async, [&, i] {
                while (!go.load())
                    this_thread::yield();
                VkPipeline p;
                check(c.create(c.modules[i % 4], (i / 4) % 2 ? shared : other, 17, &p),
                      "parallel create");
                return p;
            }));
        auto concurrentDestination = c.cache();
        auto merger = async(launch::async, [&] {
            while (!go.load())
                this_thread::yield();
            for (int i = 0; i < 12; ++i) {
                check(vkMergePipelineCaches(c.device, concurrentDestination, 1, &shared),
                      "concurrent source merge");
                c.exportCache(concurrentDestination);
            }
        });
        auto exporter = async(launch::async, [&] {
            while (!go.load())
                this_thread::yield();
            for (int i = 0; i < 12; ++i)
                c.exportCache(shared);
        });
        go = true;
        for (auto &t : tasks)
            c.pipelines.push_back(t.get());
        merger.get();
        exporter.get();
        cout << "ConcurrentSameAndDistinctModules PASS\nConcurrentSourceMergeAndExport PASS\n";
        auto bytes = c.exportCache(shared);
        auto restored = c.cache(bytes);
        auto merged = c.cache();
        check(vkMergePipelineCaches(c.device, merged, 1, &restored), "merge");
        auto roundtrip = c.exportCache(merged);
        require(!roundtrip.empty(), "empty merged cache");
        cout << "ExportImportMerge PASS\n";
        for (int i = 0; i < 4; ++i) {
            VkPipeline p;
            check(c.create(c.modules[i], merged, 17, &p), "imported create");
            c.pipelines.push_back(p);
        }
        // Cache lifetime ends before the pipelines are actually dispatched. Existing
        // pipeline results and the remaining logical views must own what they need.
        vkDestroyPipelineCache(c.device, shared, nullptr);
        c.caches[0] = VK_NULL_HANDLE;
        vector<uint32_t> expected;
        for (size_t i = 0; i < c.pipelines.size(); ++i)
            for (unsigned j = 0; j < 4; ++j)
                expected.push_back((i % 4) * 10000 + 17 + j);
        c.execute(expected);
        cout << "GpuReadbackAfterSourceCacheDestruction PASS words=" << expected.size() << '\n';
        cout << "NativeIntegration PASS pipelines=" << c.pipelines.size()
             << " serializedBytes=" << bytes.size() << '\n';
        return 0;
    } catch (const exception &e) {
        cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
