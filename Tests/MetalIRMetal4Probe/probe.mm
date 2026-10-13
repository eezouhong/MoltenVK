#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <unistd.h>

// Independent Phase 0 diagnostic. Never selects a product shader route.
static uint64_t ns() {
    static mach_timebase_info_data_t b = [] { mach_timebase_info_data_t v; mach_timebase_info(&v); return v; }();
    return (uint64_t)((__uint128_t)mach_absolute_time() * b.numer / b.denom);
}
static NSNumber* footprint() {
    task_vm_info_data_t info{}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS
        ? @(info.phys_footprint) : nil;
}
static NSDictionary* failure(NSError* error) {
    return error ? @{ @"domain": error.domain, @"code": @(error.code), @"message": error.localizedDescription }
                 : @{ @"message": @"nil result without NSError" };
}
static id<MTLLibrary> load(id<MTLDevice> device, NSString* path, NSMutableArray* loads) {
    NSData* bytes = [NSData dataWithContentsOfFile:path];
    if (!bytes) { [loads addObject:@{ @"path": path, @"success": @NO }]; return nil; }
    dispatch_data_t data = dispatch_data_create(bytes.bytes, bytes.length, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* error = nil; uint64_t start = ns();
    id<MTLLibrary> library = [device newLibraryWithData:data error:&error];
    NSMutableDictionary* row = [@{ @"path": path, @"bytes": @(bytes.length), @"loadNs": @(ns()-start), @"success": @(library != nil) } mutableCopy];
    if (!library) row[@"error"] = failure(error);
    [loads addObject:row]; return library;
}
static MTLVertexFormat format(MTLDataType type) {
    switch (type) {
        case MTLDataTypeFloat: return MTLVertexFormatFloat;
        case MTLDataTypeFloat2: return MTLVertexFormatFloat2;
        case MTLDataTypeFloat3: return MTLVertexFormatFloat3;
        case MTLDataTypeFloat4: return MTLVertexFormatFloat4;
        case MTLDataTypeInt: return MTLVertexFormatInt;
        case MTLDataTypeInt2: return MTLVertexFormatInt2;
        case MTLDataTypeInt3: return MTLVertexFormatInt3;
        case MTLDataTypeInt4: return MTLVertexFormatInt4;
        case MTLDataTypeUInt: return MTLVertexFormatUInt;
        case MTLDataTypeUInt2: return MTLVertexFormatUInt2;
        case MTLDataTypeUInt3: return MTLVertexFormatUInt3;
        case MTLDataTypeUInt4: return MTLVertexFormatUInt4;
        default: return MTLVertexFormatInvalid;
    }
}
static MTLVertexDescriptor* inferredLayout(id<MTLFunction> vertex) {
    MTLVertexDescriptor* layout = [MTLVertexDescriptor vertexDescriptor]; NSUInteger offset = 0;
    for (MTLAttribute* a in vertex.stageInputAttributes) {
        if (!a.active) continue;
        layout.attributes[a.attributeIndex].format = format(a.attributeType);
        layout.attributes[a.attributeIndex].bufferIndex = 6;
        layout.attributes[a.attributeIndex].offset = offset; offset += 16;
    }
    layout.layouts[6].stride = MAX(offset, 16); return layout;
}
static MTL4LibraryFunctionDescriptor* function(id<MTLLibrary> library, NSString* name) {
    MTL4LibraryFunctionDescriptor* f = [MTL4LibraryFunctionDescriptor new]; f.library = library; f.name = name; return f;
}
static MTL4RenderPipelineDescriptor* base(MTLRenderPipelineDescriptor* old, id<MTLLibrary> v, id<MTLLibrary> f) {
    MTL4RenderPipelineDescriptor* d = [MTL4RenderPipelineDescriptor new];
    d.vertexFunctionDescriptor = function(v, old.vertexFunction.name);
    d.fragmentFunctionDescriptor = function(f, old.fragmentFunction.name);
    d.vertexDescriptor = old.vertexDescriptor; d.rasterSampleCount = old.rasterSampleCount;
    d.inputPrimitiveTopology = old.inputPrimitiveTopology; d.rasterizationEnabled = old.rasterizationEnabled;
    d.alphaToCoverageState = MTL4AlphaToCoverageStateDisabled; d.alphaToOneState = MTL4AlphaToOneStateDisabled;
    d.maxVertexAmplificationCount = old.maxVertexAmplificationCount;
    d.supportIndirectCommandBuffers = MTL4IndirectCommandBufferSupportStateDisabled;
    d.colorAttachmentMappingState = MTL4LogicalToPhysicalColorAttachmentMappingStateInherited;
    for (NSUInteger i=0; i<8; i++) {
        auto a = d.colorAttachments[i]; a.pixelFormat = MTLPixelFormatUnspecialized;
        a.blendingState = MTL4BlendStateUnspecialized; a.writeMask = MTLColorWriteMaskUnspecialized;
        a.sourceRGBBlendFactor = a.destinationRGBBlendFactor = MTLBlendFactorUnspecialized;
        a.sourceAlphaBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
        a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationUnspecialized;
    }
    return d;
}
static void specialize(MTL4RenderPipelineDescriptor* d, MTLRenderPipelineDescriptor* old) {
    for (NSUInteger i=0; i<8; i++) {
        auto a=d.colorAttachments[i]; auto b=old.colorAttachments[i]; a.pixelFormat=b.pixelFormat;
        a.blendingState=b.blendingEnabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
        a.writeMask=b.writeMask; a.sourceRGBBlendFactor=b.sourceRGBBlendFactor;
        a.destinationRGBBlendFactor=b.destinationRGBBlendFactor; a.rgbBlendOperation=b.rgbBlendOperation;
        a.sourceAlphaBlendFactor=b.sourceAlphaBlendFactor;
        a.destinationAlphaBlendFactor=b.destinationAlphaBlendFactor; a.alphaBlendOperation=b.alphaBlendOperation;
    }
}
int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 5) return 64;
        alarm(180);
        NSString* mode=@(argv[1]); if (![mode isEqual:@"legacy"] && ![mode isEqual:@"metal4"]) return 64;
        NSArray* inputs=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[2])] options:0 error:nil];
        NSUInteger variants=strtoul(argv[4],nullptr,10); if (!inputs.count || !variants || variants>16) return 64;
        id<MTLDevice> device=MTLCreateSystemDefaultDevice(); NSError* error=nil;
        if (!device) return 69;
        if (@available(macOS 26.0, *)) {} else { return 69; }
        id<MTL4Compiler> compiler=[device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
        NSMutableArray* rows=[NSMutableArray new]; NSMutableArray* loads=[NSMutableArray new];
        char cacheDirectory[4096]={}; confstr(_CS_DARWIN_USER_CACHE_DIR,cacheDirectory,sizeof(cacheDirectory));
        NSMutableDictionary* result=[@{ @"schemaVersion":@1, @"mode":mode, @"device":device.name,
            @"os":[NSProcessInfo processInfo].operatingSystemVersionString, @"rows":rows, @"loads":loads,
            @"darwinUserCacheDirectory":@(cacheDirectory),
            @"foundationCacheDirectories":NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES),
            @"bundleIdentifier":[NSBundle mainBundle].bundleIdentifier ?: @"unbundled",
            @"metalFrameworkVersion":[NSBundle bundleForClass:[MTLRenderPipelineDescriptor class]].infoDictionary[@"CFBundleVersion"] ?: @"unavailable",
            @"descriptorScope":@"inferred vertex layout; diagnostic color variants; no captured game descriptor or pixels",
            @"footprintBeforeBytes":footprint() ?: (id)[NSNull null] } mutableCopy];
        if (!compiler) result[@"compilerError"]=failure(error);
        bool failed=false;
        const MTLPixelFormat formats[]={MTLPixelFormatRGBA32Float,MTLPixelFormatRGBA16Float,MTLPixelFormatRGBA8Unorm,MTLPixelFormatBGRA8Unorm};
        for (NSDictionary* input in inputs) {
            id<MTLLibrary> v=load(device,input[@"vertex"],loads), f=load(device,input[@"fragment"],loads);
            id<MTLFunction> vf=[v newFunctionWithName:input[@"vertexEntry"] ?: v.functionNames.firstObject];
            id<MTLFunction> ff=[f newFunctionWithName:input[@"fragmentEntry"] ?: f.functionNames.firstObject];
            if (!vf || !ff) { failed=true; continue; }
            MTLRenderPipelineDescriptor* old=[MTLRenderPipelineDescriptor new]; old.vertexFunction=vf; old.fragmentFunction=ff;
            old.vertexDescriptor=inferredLayout(vf); old.inputPrimitiveTopology=MTLPrimitiveTopologyClassTriangle;
            id<MTLRenderPipelineState> bp=nil;
            for (NSUInteger repeat=0; repeat<2; repeat++) {
                for (NSUInteger variant=0; variant<variants; variant++) {
                    old.colorAttachments[0].pixelFormat=formats[variant%4];
                    old.colorAttachments[0].blendingEnabled=(variant/4)%2;
                    old.colorAttachments[0].sourceRGBBlendFactor=MTLBlendFactorSourceAlpha;
                    old.colorAttachments[0].destinationRGBBlendFactor=MTLBlendFactorOneMinusSourceAlpha;
                    old.colorAttachments[0].writeMask=variant<8 ? MTLColorWriteMaskAll : MTLColorWriteMaskRed;
                    NSMutableDictionary* row=[@{ @"input":input[@"label"], @"variant":@(variant), @"repeat":@(repeat),
                        @"pixelFormat":@(old.colorAttachments[0].pixelFormat), @"blending":@(old.colorAttachments[0].blendingEnabled),
                        @"writeMask":@(old.colorAttachments[0].writeMask), @"baseNs":@0 } mutableCopy];
                    error=nil; uint64_t start=ns(); id<MTLRenderPipelineState> p=nil;
                    if ([mode isEqual:@"legacy"]) {
                        p=[device newRenderPipelineStateWithDescriptor:old error:&error]; row[@"legacyNs"]=@(ns()-start);
                    } else {
                        if (!bp) {
                            bp=[compiler newRenderPipelineStateWithDescriptor:base(old,v,f) compilerTaskOptions:nil error:&error];
                            row[@"baseNs"]=@(ns()-start);
                            if (!bp) { row[@"baseError"]=failure(error); }
                        }
                        if (bp) {
                            MTL4RenderPipelineDescriptor* d=(MTL4RenderPipelineDescriptor*)[bp newRenderPipelineDescriptorForSpecialization];
                            specialize(d,old); uint64_t s=ns();
                            p=[compiler newRenderPipelineStateBySpecializationWithDescriptor:d pipeline:bp error:&error];
                            row[@"specializationNs"]=@(ns()-s);
                        }
                    }
                    row[@"totalNs"]=@(ns()-start); row[@"success"]=@(p!=nil);
                    row[@"footprintBytes"]=footprint() ?: (id)[NSNull null];
                    if (!p) { row[@"error"]=failure(error); failed=true; }
                    [rows addObject:row];
                    // A compatibility failure invalidates the B timing denominator.
                    // Preserve it once per input instead of measuring repeated failures.
                    if (!p) break;
                }
                if (failed && [mode isEqual:@"metal4"] && !bp) break;
            }
            if (input[@"compute"]) {
                id<MTLLibrary> c=load(device,input[@"compute"],loads);
                id<MTLFunction> cf=[c newFunctionWithName:input[@"computeEntry"] ?: c.functionNames.firstObject];
                for (NSUInteger repeat=0; repeat<2; repeat++) {
                    error=nil; uint64_t start=ns(); id<MTLComputePipelineState> p=nil;
                    if ([mode isEqual:@"legacy"]) {
                        p=cf ? [device newComputePipelineStateWithFunction:cf error:&error] : nil;
                    } else {
                        MTL4ComputePipelineDescriptor* d=[MTL4ComputePipelineDescriptor new];
                        d.computeFunctionDescriptor=function(c,cf.name);
                        p=[compiler newComputePipelineStateWithDescriptor:d compilerTaskOptions:nil error:&error];
                    }
                    NSMutableDictionary* row=[@{ @"input":input[@"label"], @"kind":@"compute", @"repeat":@(repeat),
                        @"totalNs":@(ns()-start), @"success":@(p!=nil), @"footprintBytes":footprint() ?: (id)[NSNull null] } mutableCopy];
                    if (!p) { row[@"error"]=failure(error); failed=true; }
                    [rows addObject:row];
                }
            }
        }
        result[@"footprintAfterBytes"]=footprint() ?: (id)[NSNull null]; result[@"success"]=@(!failed);
        NSData* data=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
        if (![data writeToFile:@(argv[3]) options:NSDataWritingWithoutOverwriting error:nil]) return 73;
        printf("mode=%s rows=%lu success=%s\n",argv[1],(unsigned long)rows.count,failed ? "false":"true");
        return failed ? 1:0;
    }
}
