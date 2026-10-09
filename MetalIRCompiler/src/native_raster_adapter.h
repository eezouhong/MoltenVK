#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace melonx::air {
enum NativeRasterIO : uint32_t { PointSize=1,PointCoordinates=2,VertexID=4,InstanceID=8 };
// Pure compiler transformation. No Metal device, Vulkan state or persistence.
bool restoreNativeRasterIO(const void* metallib,size_t size,uint32_t execution,
                          uint32_t requiredIO,std::vector<uint8_t>& output,
                          std::string& error);
}
