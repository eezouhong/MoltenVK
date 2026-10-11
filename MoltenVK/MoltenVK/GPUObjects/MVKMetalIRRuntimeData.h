#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mvkir {

// Preserves Mesa static-CBV offsets. ABI7 uploads zeros in the first8bytes;
// draw bases are supplied separately through a four-byte-aligned raw SRV.
// All padding is explicit so copying this object never publishes stale bytes.
struct VertexData {
    uint32_t firstVertex = 0;
    uint32_t baseInstance = 0;
    uint32_t isIndexedDraw = 0;
    uint32_t yzFlipMask = 0;
    uint32_t drawId = 0;
    float viewportWidth = 0;
    float viewportHeight = 0;
    uint32_t viewIndex = 0;
    float depthBias = 0;
};

// Preserves Mesa compute static-CBV offsets. ABI7 group counts use a raw SRV;
// these host input fields are zeroed when uploading the static CBV.
struct ComputeData {
    uint32_t groupCount[3] = {};
    uint32_t padding = 0;
    uint32_t baseGroup[3] = {};
};

// Mirrors the pinned MSC runtime's 20-byte draw/drawIndexed union. The indexed
// start-index field uses the byte offset passed to Metal, as its SDK helper does.
struct DrawArguments {
    uint32_t words[5] = {};
};

struct DirectDraw {
    VertexData mesa;
    DrawArguments metal;
    uint16_t indexType = 0; // MSC: 0 non-indexed, Metal index enum + 1 otherwise.
};

inline DirectDraw makeDraw(uint32_t count, uint32_t instances, uint32_t firstVertex,
                           uint32_t firstInstance, uint32_t drawId = 0) {
    DirectDraw value{};
    value.mesa.firstVertex = firstVertex;
    value.mesa.baseInstance = firstInstance;
    value.mesa.drawId = drawId;
    value.metal.words[0] = count;
    value.metal.words[1] = instances;
    value.metal.words[2] = firstVertex;
    value.metal.words[3] = firstInstance;
    return value;
}

inline DirectDraw makeIndexedDraw(uint32_t count, uint32_t instances,
                                  uint32_t metalIndexByteOffset, int32_t baseVertex,
                                  uint32_t firstInstance, uint16_t metalIndexType,
                                  uint32_t drawId = 0) {
    DirectDraw value{};
    value.mesa.firstVertex = static_cast<uint32_t>(baseVertex);
    value.mesa.baseInstance = firstInstance;
    value.mesa.isIndexedDraw = 1;
    value.mesa.drawId = drawId;
    value.metal.words[0] = count;
    value.metal.words[1] = instances;
    value.metal.words[2] = metalIndexByteOffset;
    value.metal.words[3] = static_cast<uint32_t>(baseVertex);
    value.metal.words[4] = firstInstance;
    value.indexType = metalIndexType + 1;
    return value;
}

static_assert(std::is_trivially_copyable_v<VertexData>);
static_assert(std::is_trivially_copyable_v<ComputeData> && sizeof(ComputeData) == 28);
static_assert(std::is_trivially_copyable_v<DrawArguments>);
static_assert(sizeof(VertexData) == 36 && sizeof(DrawArguments) == 20);
static_assert(offsetof(VertexData, firstVertex) == 0);
static_assert(offsetof(VertexData, baseInstance) == 4);
static_assert(offsetof(VertexData, isIndexedDraw) == 8);
static_assert(offsetof(VertexData, yzFlipMask) == 12);
static_assert(offsetof(VertexData, drawId) == 16);
static_assert(offsetof(VertexData, viewIndex) == 28);

} // namespace mvkir
