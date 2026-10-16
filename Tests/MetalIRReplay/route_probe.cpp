#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 3) return 64;
    auto get = reinterpret_cast<uint32_t(*)(uint32_t*,uint32_t)>(
        dlsym(RTLD_DEFAULT, "vkGetShaderCompilerRouteMVK"));
    auto abi = reinterpret_cast<uint32_t(*)()>(dlsym(RTLD_DEFAULT,"vkGetMetalIRCompilerABIMVK"));
    if (!get || !abi) return 1;
    // Simulate Swift's prewarm: validating ABI must not freeze the route before
    // the launch request has been applied.
    if (!strcmp(argv[2], "prewarm")) (void)abi();
    setenv("MELONX_EXPERIMENTAL_METAL_IR", argv[1], 1);
    std::array<uint32_t,5> before{}, after{};
    if (get(before.data(), before.size()) != before.size()) return 1;
    uint32_t compilerABI = abi();
    VkInstance instance{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo=&app;
    if (vkCreateInstance(&info,nullptr,&instance) != VK_SUCCESS) return 1;
    setenv("MELONX_EXPERIMENTAL_METAL_IR", !strcmp(argv[1],"1") ? "0" : "1", 1);
    setenv("MELONX_METAL_IR_PLUGIN", "/missing/late-plugin.dylib", 1);
    uint32_t devices=0;
    auto result=vkEnumeratePhysicalDevices(instance,&devices,nullptr);
    bool tessellation=false;
    if (result == VK_SUCCESS && devices) {
        VkPhysicalDevice physical{}; uint32_t count=1;
        if (vkEnumeratePhysicalDevices(instance,&count,&physical) != VK_SUCCESS) return 1;
        VkPhysicalDeviceFeatures features{};vkGetPhysicalDeviceFeatures(physical,&features);
        tessellation=features.tessellationShader;
    }
    bool same=get(after.data(),after.size())==after.size() && before==after;
    vkDestroyInstance(instance,nullptr);
    if (result != VK_SUCCESS || !devices || !same) return 1;
    std::cout << "{\"immutable\":true,\"requested\":" << before[1]
              << ",\"available\":" << before[2] << ",\"selected\":" << before[3]
              << ",\"reason\":" << before[4] << ",\"physicalDevices\":" << devices
              << ",\"compilerABI\":" << compilerABI << ",\"tessellation\":" << tessellation << "}\n";
}
