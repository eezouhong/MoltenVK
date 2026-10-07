/* Experimental compiler ABI. The optional compiler plugin owns Mesa/MSC;
 * MoltenVK owns pipeline selection, resource lifetimes and Vulkan encoding. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum : uint32_t { MVK_METAL_IR_ABI_VERSION = 7 };
enum : uint32_t { MVK_METAL_IR_FLIP_Y = 1, MVK_METAL_IR_CLIP_HALF_Z = 2 };
enum : uint32_t { MVK_METAL_IR_RUNTIME_DATA = 1, MVK_METAL_IR_DRAW_PARAMETERS = 2 };
enum : uint32_t { MVK_METAL_IR_UNIT_POINT_SIZE = 4 };
enum : uint32_t { MVK_METAL_IR_DRAW_BASES = 32, MVK_METAL_IR_DISPATCH_GROUPS = 64 };
enum : uint32_t { MVK_METAL_IR_NATIVE_POINT_SIZE = 8, MVK_METAL_IR_NATIVE_POINT_COORDINATES = 16 };
enum : uint32_t { MVK_METAL_IR_ALLOW_DISPATCH_BASE = 1, MVK_METAL_IR_RENDERING_POINTS = 2 };
struct MVKMetalIRBinding {
    uint32_t set;
    uint32_t binding;
    uint32_t count;
    uint32_t descriptorType;
    uint32_t denseIndex;
};
struct MVKMetalIRCompileRequest {
    uint32_t abiVersion;
    uint32_t executionModel;
    const uint32_t* words;
    size_t wordCount;
    const char* entry;
    const MVKMetalIRBinding* bindings;
    size_t bindingCount;
    const uint32_t* setSizes;
    uint32_t setCount;
    uint32_t pushConstantSize;
    uint32_t preserveInvariance;
    uint32_t strictMath;
    uint32_t vertexTransformFlags;
    uint32_t runtimeOptions;
};
struct MVKMetalIRCompileResult {
    uint32_t abiVersion;
    uint32_t status;
    void* metallib;
    size_t metallibSize;
    char entry[256];
    uint32_t threadgroupSize[3];
    uint64_t usedSets;
    uint64_t vertexLocations;
    uint8_t vertexAttributes[32];
    uint32_t runtimeFlags;
    double mesaMs;
    double converterMs;
    double rasterAdapterMs;
    char error[512];
};
using MVKMetalIRCompileFunction = int(*)(const MVKMetalIRCompileRequest*, MVKMetalIRCompileResult*);
using MVKMetalIRReleaseFunction = void(*)(MVKMetalIRCompileResult*);
