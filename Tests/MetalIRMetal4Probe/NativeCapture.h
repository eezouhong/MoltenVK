#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>

// Compile-time opt-in evidence capture. It never creates a Metal 4 PSO.
namespace mvkirphase0 {
inline const char* directory() {
    const char* value=std::getenv("MELONX_IR_PHASE0_DUMP");
    return value && *value ? value : nullptr;
}
inline void library(const std::string& key, const void* bytes, size_t size) {
    const char* dir=directory(); if(!dir)return;
    @autoreleasepool {
        NSString* path=[NSString stringWithFormat:@"%s/%s.metallib",dir,key.c_str()];
        if([[NSFileManager defaultManager] fileExistsAtPath:path])return;
        NSError* error=nil;
        if(![[NSData dataWithBytes:bytes length:size] writeToFile:path
                options:NSDataWritingWithoutOverwriting error:&error])
            fprintf(stderr,"IR phase0 library dump failed: %s\n",error.localizedDescription.UTF8String ?: "unknown");
    }
}
inline void render(MTLRenderPipelineDescriptor* descriptor,
                   const std::string& vertexKey,const std::string& fragmentKey,
                   const std::string& baseKey) {
    const char* dir=directory(); if(!dir)return;
    static std::mutex captureMutex;
    static std::unordered_map<std::string,uint64_t> bases;
    static uint64_t requests=0;
    std::lock_guard<std::mutex> guard(captureMutex);
    ++requests; ++bases[baseKey];
    @autoreleasepool {
        NSMutableArray* attributes=[NSMutableArray array];
        NSMutableArray* layouts=[NSMutableArray array];
        NSMutableArray* colors=[NSMutableArray array];
        if(descriptor.vertexDescriptor) {
            for(NSUInteger i=0;i<31;i++) {
                auto a=descriptor.vertexDescriptor.attributes[i];
                [attributes addObject:@{@"format":@(a.format),@"offset":@(a.offset),@"bufferIndex":@(a.bufferIndex)}];
                auto l=descriptor.vertexDescriptor.layouts[i];
                [layouts addObject:@{@"stride":@(l.stride),@"stepFunction":@(l.stepFunction),@"stepRate":@(l.stepRate)}];
            }
        }
        for(NSUInteger i=0;i<8;i++) {
            auto a=descriptor.colorAttachments[i];
            [colors addObject:@{@"pixelFormat":@(a.pixelFormat),@"blendingEnabled":@(a.blendingEnabled),
                @"writeMask":@(a.writeMask),@"sourceRGBBlendFactor":@(a.sourceRGBBlendFactor),
                @"destinationRGBBlendFactor":@(a.destinationRGBBlendFactor),@"rgbBlendOperation":@(a.rgbBlendOperation),
                @"sourceAlphaBlendFactor":@(a.sourceAlphaBlendFactor),
                @"destinationAlphaBlendFactor":@(a.destinationAlphaBlendFactor),@"alphaBlendOperation":@(a.alphaBlendOperation)}];
        }
        NSData* keyData=[NSData dataWithBytes:baseKey.data() length:baseKey.size()];
        NSDictionary* row=@{@"schemaVersion":@1,@"request":@(requests),@"uniqueBases":@(bases.size()),
            @"baseKeyBase64":[keyData base64EncodedStringWithOptions:0],
            @"vertexKey":@(vertexKey.c_str()),@"fragmentKey":@(fragmentKey.c_str()),
            @"vertex":[NSString stringWithFormat:@"%s/%s.metallib",dir,vertexKey.c_str()],
            @"fragment":fragmentKey.empty() ? (id)[NSNull null] : [NSString stringWithFormat:@"%s/%s.metallib",dir,fragmentKey.c_str()],
            @"vertexEntry":descriptor.vertexFunction.name ?: @"",
            @"fragmentEntry":descriptor.fragmentFunction.name ?: @"",
            @"vertexDescriptor":@{@"present":@(descriptor.vertexDescriptor!=nil),@"attributes":attributes,@"layouts":layouts},
            @"state":@{@"rasterSampleCount":@(descriptor.rasterSampleCount),
                @"alphaToCoverageEnabled":@(descriptor.alphaToCoverageEnabled),@"alphaToOneEnabled":@(descriptor.alphaToOneEnabled),
                @"rasterizationEnabled":@(descriptor.rasterizationEnabled),@"maxVertexAmplificationCount":@(descriptor.maxVertexAmplificationCount),
                @"inputPrimitiveTopology":@(descriptor.inputPrimitiveTopology),@"supportIndirectCommandBuffers":@(descriptor.supportIndirectCommandBuffers),
                @"shaderValidation":@(descriptor.shaderValidation),@"depthAttachmentPixelFormat":@(descriptor.depthAttachmentPixelFormat),
                @"stencilAttachmentPixelFormat":@(descriptor.stencilAttachmentPixelFormat),@"colors":colors}};
        NSData* encoded=[NSJSONSerialization dataWithJSONObject:row options:0 error:nullptr];
        std::string path=std::string(dir)+"/render-descriptors.jsonl";
        FILE* file=fopen(path.c_str(),"ab");
        bool good=file && encoded;
        if(good) good=fwrite(encoded.bytes,1,encoded.length,file)==encoded.length && fputc('\n',file)!=EOF;
        if(file)fclose(file);
        if(!good)fprintf(stderr,"IR phase0 render descriptor dump failed\n");
        fprintf(stderr,"MELONX_IR_PHASE0_BASE {\"requests\":%llu,\"uniqueBases\":%zu}\n",
            (unsigned long long)requests,bases.size());
    }
}
inline void compute(MTLComputePipelineDescriptor* descriptor,const std::string& key) {
    const char* dir=directory(); if(!dir)return;
    static std::mutex captureMutex;
    std::lock_guard<std::mutex> guard(captureMutex);
    @autoreleasepool {
        auto required=descriptor.requiredThreadsPerThreadgroup;
        NSDictionary* row=@{@"schemaVersion":@1,@"kind":@"compute",@"computeKey":@(key.c_str()),
            @"compute":[NSString stringWithFormat:@"%s/%s.metallib",dir,key.c_str()],
            @"computeEntry":descriptor.computeFunction.name ?: @"",
            @"state":@{@"maxTotalThreadsPerThreadgroup":@(descriptor.maxTotalThreadsPerThreadgroup),
                @"threadGroupSizeIsMultipleOfThreadExecutionWidth":@(descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth),
                @"requiredThreadsPerThreadgroup":@[@(required.width),@(required.height),@(required.depth)],
                @"supportIndirectCommandBuffers":@(descriptor.supportIndirectCommandBuffers),
                @"shaderValidation":@(descriptor.shaderValidation)}};
        NSData* encoded=[NSJSONSerialization dataWithJSONObject:row options:0 error:nullptr];
        std::string path=std::string(dir)+"/compute-descriptors.jsonl";
        FILE* file=fopen(path.c_str(),"ab");
        bool good=file && encoded;
        if(good)good=fwrite(encoded.bytes,1,encoded.length,file)==encoded.length && fputc('\n',file)!=EOF;
        if(file)fclose(file);
        if(!good)fprintf(stderr,"IR phase0 compute descriptor dump failed\n");
    }
}
}
