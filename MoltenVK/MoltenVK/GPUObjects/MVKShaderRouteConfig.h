#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <limits>

enum class MVKShaderCompilerRoute : uint32_t { MSL, MetalIR, Unavailable };
enum class MVKShaderCompilerAvailability : uint32_t { Unavailable, Available, NotProbed };

struct MVKShaderRouteConfig {
    bool requested = false;
    MVKShaderCompilerAvailability available = MVKShaderCompilerAvailability::NotProbed;
    MVKShaderCompilerRoute selected = MVKShaderCompilerRoute::MSL;
    uint32_t failureReason = 0; // 0 none, 1 plugin/ABI, 2 hardware.
    // Preserve explicit IR's existing failure path: unavailable never means
    // silently compiling MSL. Swift still gates an unavailable launch.
    bool usesIRPath() const { return requested; }
};

const MVKShaderRouteConfig& mvkShaderRouteConfig();

// Central environment access, preserving each setting's original capture point.
// Workers and plugin are captured by their existing process-static consumers;
// telemetry's initial value is captured by its mutable diagnostics storage.
unsigned mvkShaderCompilerWorkerLimit();
const std::string& mvkShaderCompilerPluginPath();
bool mvkShaderInitialTelemetryEnabled();
struct MVKShaderIRResidentLimits { size_t count; size_t bytes; };
constexpr MVKShaderIRResidentLimits mvkShaderIRResidentLimits(uint32_t sharedLimit, bool disabled) {
    if (disabled) return {0, 0};
    if (!sharedLimit) return {std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()};
    return {sharedLimit, 64u * 1024u * 1024u};
}
struct MVKShaderDeviceCacheConfig { const char* directory; size_t retainedCount; size_t retainedBytes; };
MVKShaderDeviceCacheConfig mvkShaderDeviceCacheConfig(uint32_t sharedResidentLimit);
struct MVKShaderCompileConfig { const char* stages; const char* strictMath; };
MVKShaderCompileConfig mvkShaderCompileConfig();
