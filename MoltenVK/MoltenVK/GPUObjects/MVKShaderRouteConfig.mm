#include "MVKShaderRouteConfig.h"
#include "MVKMetalIR.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>

const MVKShaderRouteConfig& mvkShaderRouteConfig() {
    static const MVKShaderRouteConfig config = [] {
        MVKShaderRouteConfig result;
        const char* request = getenv("MELONX_EXPERIMENTAL_METAL_IR");
        result.requested = request && !strcmp(request, "1");
        if (result.requested) {
            result.available = MVKShaderCompilerAvailability::Unavailable;
            result.selected = MVKShaderCompilerRoute::Unavailable;
            if (!mvkMetalIRCompilerAvailable()) result.failureReason = 1;
            else {
                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                bool hardware = device && [device supportsFamily:MTLGPUFamilyMetal3] &&
                                device.argumentBuffersSupport == MTLArgumentBuffersTier2;
                [device release];
                if (!hardware) result.failureReason = 2;
                else {
                    result.available = MVKShaderCompilerAvailability::Available;
                    result.selected = MVKShaderCompilerRoute::MetalIR;
                }
            }
        }
        fprintf(stderr, "MELONX_SHADER_ROUTE requested=%u available=%u selected=%u reason=%u; immutable process selection, automatic MSL fallback disabled\n",
                result.requested, unsigned(result.available), unsigned(result.selected), result.failureReason);
        return result;
    }();
    return config;
}

unsigned mvkShaderCompilerWorkerLimit() {
    const char* value = getenv("MELONX_METAL_IR_COMPILE_WORKERS");
    if (!value || !*value) return 2u;
    char* end = nullptr;
    unsigned long count = strtoul(value, &end, 10);
    return end && !*end && count >= 1 && count <= 8 ? unsigned(count) : 2u;
}
const std::string& mvkShaderCompilerPluginPath() {
    static const std::string path = [] {
        const char* value = getenv("MELONX_METAL_IR_PLUGIN");
        return std::string(value ? value : "");
    }();
    return path;
}
bool mvkShaderInitialTelemetryEnabled() {
    const char* value = getenv("MELONX_METAL_IR_TELEMETRY");
    return value && !strcmp(value, "1");
}
MVKShaderDeviceCacheConfig mvkShaderDeviceCacheConfig(uint32_t sharedResidentLimit) {
    const char* value = getenv("MELONX_METAL_IR_MEMORY_CACHE");
    // This independent diagnostic switch disables retention. The shared MSL/IR
    // resident-count setting instead defines zero as unlimited.
    const auto limits = mvkShaderIRResidentLimits(sharedResidentLimit, value && !strcmp(value, "0"));
    return {getenv("MELONX_METAL_IR_CACHE"), limits.count, limits.bytes};
}
MVKShaderCompileConfig mvkShaderCompileConfig() {
    return {getenv("MELONX_METAL_IR_STAGES"), getenv("MELONX_METAL_IR_STRICT_MATH")};
}
