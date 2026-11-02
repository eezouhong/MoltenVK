#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace melonx::air {
enum NativeRasterIO : uint32_t { PointSize=1,PointCoordinates=2,VertexID=4,InstanceID=8,PatchID=16,PositionInPatch=32 };
// Metal [[patch(...)]] domains, as the metallib TESS tag encodes them.
enum PostTessellationDomain : uint32_t { TrianglePatch=1,QuadPatch=2 };
// Pure compiler transformation. No Metal device, Vulkan state or persistence.
bool restoreNativeRasterIO(const void* metallib,size_t size,uint32_t execution,
                          uint32_t requiredIO,std::vector<uint8_t>& output,
                          std::string& error);
// Static rasterizer discard still executes the complete vertex body. Metal
// requires a void entry and no raster outputs for that pipeline variant.
// Metal post-tessellation vertex function. Mesa's hidden patch markers become
// [[patch_id]] and [[position_in_patch]]; the AIR entry gains its
// [[patch(domain, 0)]] attribute and the metallib function its TESS tag.
// requiredIO must hold PositionInPatch and no VertexID or PointSize.
bool makePostTessellationVertex(const void* metallib,size_t size,uint32_t domain,
                                uint32_t requiredIO,std::vector<uint8_t>& output,
                                std::string& error);
bool discardVertexOutputs(const void* metallib,size_t size,
                          std::vector<uint8_t>& output,std::string& error);
}
