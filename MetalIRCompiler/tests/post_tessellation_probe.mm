// GPU proof for MVK_METAL_IR_POST_TESSELLATION_*: a SPIR-V vertex stage that reads PrimitiveId and TessCoord is
// compiled by the plugin into a Metal post-tessellation vertex function and drawn with drawPatches.
// usage: post_tessellation_probe PLUGIN.dylib SHADER.spv quad|triangle FACTOR
// The shader (post_tessellation.vert) reads patch rectangles from a storage buffer at set 0 binding 0, places the
// domain point in its patch rectangle (quads: x by u, y by v; triangles: u, v, w weight three corners) and outputs
// float4(TessCoord, PrimitiveId). The oracle samples every covered pixel: the color must be the domain coordinate of
// the pixel center under that affine mapping (exact for any tessellation of a flat patch), and the patch index must
// be the patch whose rectangle holds the pixel. Prints one JSON line.
#import <Metal/Metal.h>
#include "MVKMetalIRBridge.h"
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static const char* fragmentSource =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct In { float4 position [[position]]; float4 color [[user(texcoord0)]]; };\n"
    "fragment float4 probe_fragment(In in [[stage_in]]) { return in.color; }\n";

static int fail(const char* message, const char* detail = "") {
    printf("{\"passed\":false,\"error\":\"%s %s\"}\n", message, detail);
    return 1;
}

int main(int argc, char** argv) {
    @autoreleasepool {
        if (argc != 5) return fail("usage");
        bool quads = !strcmp(argv[3], "quad");
        float factor = (float)atof(argv[4]);
        void* plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!plugin) return fail("dlopen", dlerror());
        auto compile = (MVKMetalIRCompileFunction)dlsym(plugin, "MeloNXCompileMetalIR");
        auto release = (MVKMetalIRReleaseFunction)dlsym(plugin, "MeloNXReleaseMetalIR");
        if (!compile || !release) return fail("plugin exports");
        std::ifstream file(argv[2], std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
        std::vector<uint32_t> words(bytes.size() / 4);
        memcpy(words.data(), bytes.data(), words.size() * 4);

        // One storage buffer: SRV and UAV kinds, table entries 0 and 1 of set 0.
        MVKMetalIRBinding binding = {0, 0, 1, 7, 0, {UINT32_MAX, 0, 1, UINT32_MAX}};
        uint32_t setSize = 2;
        MVKMetalIRCompileRequest request = {};
        request.abiVersion = MVK_METAL_IR_ABI_VERSION;
        request.executionModel = 0;
        request.words = words.data();
        request.wordCount = words.size();
        request.entry = "main";
        request.bindings = &binding;
        request.bindingCount = 1;
        request.setSizes = &setSize;
        request.setCount = 1;
        request.mathMode = MVK_METAL_IR_MATH_SAFE;
        request.runtimeOptions = quads ? MVK_METAL_IR_POST_TESSELLATION_QUADS : MVK_METAL_IR_POST_TESSELLATION_TRIANGLES;
        MVKMetalIRCompileResult result = {};
        if (compile(&request, &result) != 0) return fail("compile", result.error);
        if (!(result.runtimeFlags & MVK_METAL_IR_POST_TESSELLATION_VERTEX)) return fail("result flag missing");

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        NSError* error = nil;
        dispatch_data_t data = dispatch_data_create(result.metallib, result.metallibSize, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
        if (!library) return fail("library", error.localizedDescription.UTF8String);
        id<MTLFunction> vertex = [library newFunctionWithName:[NSString stringWithUTF8String:result.entry]];
        release(&result);
        if (!vertex) return fail("vertex function");
        MTLPatchType expectedType = quads ? MTLPatchTypeQuad : MTLPatchTypeTriangle;
        if (vertex.patchType != expectedType || vertex.patchControlPointCount != 0) return fail("patch type");
        id<MTLLibrary> fragmentLibrary = [device newLibraryWithSource:[NSString stringWithUTF8String:fragmentSource] options:nil error:&error];
        if (!fragmentLibrary) return fail("fragment", error.localizedDescription.UTF8String);

        MTLRenderPipelineDescriptor* descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = vertex;
        descriptor.fragmentFunction = [fragmentLibrary newFunctionWithName:@"probe_fragment"];
        descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        descriptor.maxTessellationFactor = 64;
        descriptor.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
        descriptor.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        descriptor.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
        descriptor.tessellationPartitionMode = factor == floorf(factor) ? MTLTessellationPartitionModeInteger
                                                                         : MTLTessellationPartitionModeFractionalOdd;
        descriptor.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
        id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (!pipeline) return fail("pipeline", error.localizedDescription.UTF8String);

        // Two patch rectangles in clip space (x0, y0, x1, y1).
        const float rects[2][4] = {{-0.9f, -0.9f, -0.1f, 0.7f}, {0.1f, -0.5f, 0.9f, 0.9f}};
        id<MTLBuffer> patches = [device newBufferWithBytes:rects length:sizeof(rects) options:MTLResourceStorageModeShared];
        uint16_t factors[2][6];
        for (auto& patch : factors) {
            __fp16 value = (__fp16)factor;
            for (auto& f : patch) memcpy(&f, &value, 2);
        }
        id<MTLBuffer> factorBuffer = [device newBufferWithBytes:factors length:sizeof(factors) options:MTLResourceStorageModeShared];
        struct Entry { uint64_t va, view, metadata; } table[2] = {{patches.gpuAddress, 0, sizeof(rects)}, {patches.gpuAddress, 0, sizeof(rects)}};
        id<MTLBuffer> tableBuffer = [device newBufferWithBytes:table length:sizeof(table) options:MTLResourceStorageModeShared];
        uint64_t top[2] = {tableBuffer.gpuAddress, 0};
        id<MTLBuffer> topBuffer = [device newBufferWithBytes:top length:sizeof(top) options:MTLResourceStorageModeShared];
        id<MTLBuffer> zeros = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];

        const int size = 64;
        MTLTextureDescriptor* textureDescriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float
                                                                                                    width:size height:size mipmapped:NO];
        textureDescriptor.usage = MTLTextureUsageRenderTarget;
        textureDescriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:textureDescriptor];
        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(-1, -1, -1, -1);
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:topBuffer offset:0 atIndex:2];
        [encoder setVertexBuffer:zeros offset:0 atIndex:0];
        [encoder setVertexBuffer:zeros offset:0 atIndex:1];
        [encoder setVertexBuffer:zeros offset:0 atIndex:4];
        [encoder setVertexBuffer:zeros offset:0 atIndex:5];
        [encoder useResource:patches usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [encoder useResource:tableBuffer usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
        [encoder setTessellationFactorBuffer:factorBuffer offset:0 instanceStride:0];
        [encoder drawPatches:0 patchStart:0 patchCount:2 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        if (commands.error) return fail("gpu", commands.error.localizedDescription.UTF8String);

        std::vector<float> pixels(size * size * 4);
        [target getBytes:pixels.data() bytesPerRow:size * 16 fromRegion:MTLRegionMake2D(0, 0, size, size) mipmapLevel:0];
        int covered = 0, expected = 0, wrongPatch = 0, outside = 0;
        double maxError = 0;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const float* c = &pixels[(y * size + x) * 4];
                // Pixel center in clip space; texture row 0 is the top (clip y = +1).
                float cx = (x + 0.5f) / size * 2 - 1, cy = 1 - (y + 0.5f) / size * 2;
                int inside = -1;
                float u = 0, v = 0;
                for (int p = 0; p < 2; ++p) {
                    const float* r = rects[p];
                    float pu = (cx - r[0]) / (r[2] - r[0]), pv = (cy - r[1]) / (r[3] - r[1]);
                    // Triangles cover the half of the rectangle with u + v <= 1 (third corner at u = v = 0).
                    bool in = pu > 0.01f && pu < 0.99f && pv > 0.01f && pv < 0.99f && (quads || pu + pv < 0.98f);
                    if (in) { inside = p; u = pu; v = pv; }
                }
                bool hit = c[3] >= 0;
                covered += hit;
                if (inside < 0) { outside += hit && c[3] >= 0 && false; continue; }
                ++expected;
                if (!hit) continue;
                if ((int)lroundf(c[3]) != inside) { ++wrongPatch; continue; }
                double e = std::max<double>(fabs(c[0] - u), fabs(c[1] - v));
                if (!quads) e = std::max<double>(e, fabs(c[2] - (1 - u - v)));
                maxError = std::max(maxError, e);
            }
        }
        int missing = 0;
        for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
            float cx = (x + 0.5f) / size * 2 - 1, cy = 1 - (y + 0.5f) / size * 2;
            for (int p = 0; p < 2; ++p) {
                const float* r = rects[p];
                float pu = (cx - r[0]) / (r[2] - r[0]), pv = (cy - r[1]) / (r[3] - r[1]);
                bool in = pu > 0.01f && pu < 0.99f && pv > 0.01f && pv < 0.99f && (quads || pu + pv < 0.98f);
                if (in && pixels[(y * size + x) * 4 + 3] < 0) ++missing;
            }
        }
        bool passed = missing == 0 && wrongPatch == 0 && maxError < 2e-3;
        printf("{\"passed\":%s,\"domain\":\"%s\",\"factor\":%g,\"expectedPixels\":%d,\"coveredPixels\":%d,\"missing\":%d,"
               "\"wrongPatch\":%d,\"maxCoordinateError\":%.6f}\n",
               passed ? "true" : "false", argv[3], factor, expected, covered, missing, wrongPatch, maxError);
        (void)outside;
        return passed ? 0 : 1;
    }
}
