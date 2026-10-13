// Reuse the validated standalone helpers; the synthetic main is not executed.
#define main diagnostic_main
#include "probe.mm"
#undef main

static MTLRenderPipelineDescriptor* restoreRender(NSDictionary* row,id<MTLLibrary> vertex,id<MTLLibrary> fragment) {
    MTLRenderPipelineDescriptor* d=[MTLRenderPipelineDescriptor new];
    d.vertexFunction=[vertex newFunctionWithName:row[@"vertexEntry"]];
    d.fragmentFunction=fragment ? [fragment newFunctionWithName:row[@"fragmentEntry"]] : nil;
    NSDictionary* layout=row[@"vertexDescriptor"];
    if([layout[@"present"] boolValue]) {
        MTLVertexDescriptor* v=[MTLVertexDescriptor vertexDescriptor];
        NSArray* attributes=layout[@"attributes"]; NSArray* layouts=layout[@"layouts"];
        for(NSUInteger i=0;i<attributes.count;i++) {
            NSDictionary* a=attributes[i]; v.attributes[i].format=(MTLVertexFormat)[a[@"format"] unsignedIntegerValue];
            v.attributes[i].offset=[a[@"offset"] unsignedIntegerValue]; v.attributes[i].bufferIndex=[a[@"bufferIndex"] unsignedIntegerValue];
        }
        for(NSUInteger i=0;i<layouts.count;i++) {
            NSDictionary* a=layouts[i]; v.layouts[i].stride=[a[@"stride"] unsignedIntegerValue];
            v.layouts[i].stepFunction=(MTLVertexStepFunction)[a[@"stepFunction"] unsignedIntegerValue];
            v.layouts[i].stepRate=[a[@"stepRate"] unsignedIntegerValue];
        }
        d.vertexDescriptor=v;
    }
    NSDictionary* s=row[@"state"];
    d.rasterSampleCount=[s[@"rasterSampleCount"] unsignedIntegerValue];
    d.alphaToCoverageEnabled=[s[@"alphaToCoverageEnabled"] boolValue];
    d.alphaToOneEnabled=[s[@"alphaToOneEnabled"] boolValue];
    d.rasterizationEnabled=[s[@"rasterizationEnabled"] boolValue];
    d.maxVertexAmplificationCount=[s[@"maxVertexAmplificationCount"] unsignedIntegerValue];
    d.inputPrimitiveTopology=(MTLPrimitiveTopologyClass)[s[@"inputPrimitiveTopology"] unsignedIntegerValue];
    d.supportIndirectCommandBuffers=[s[@"supportIndirectCommandBuffers"] boolValue];
    d.shaderValidation=(MTLShaderValidation)[s[@"shaderValidation"] unsignedIntegerValue];
    d.depthAttachmentPixelFormat=(MTLPixelFormat)[s[@"depthAttachmentPixelFormat"] unsignedIntegerValue];
    d.stencilAttachmentPixelFormat=(MTLPixelFormat)[s[@"stencilAttachmentPixelFormat"] unsignedIntegerValue];
    NSArray* colors=s[@"colors"];
    for(NSUInteger i=0;i<colors.count;i++) {
        NSDictionary* a=colors[i]; auto c=d.colorAttachments[i];
        c.pixelFormat=(MTLPixelFormat)[a[@"pixelFormat"] unsignedIntegerValue];
        c.blendingEnabled=[a[@"blendingEnabled"] boolValue]; c.writeMask=(MTLColorWriteMask)[a[@"writeMask"] unsignedIntegerValue];
        c.sourceRGBBlendFactor=(MTLBlendFactor)[a[@"sourceRGBBlendFactor"] unsignedIntegerValue];
        c.destinationRGBBlendFactor=(MTLBlendFactor)[a[@"destinationRGBBlendFactor"] unsignedIntegerValue];
        c.rgbBlendOperation=(MTLBlendOperation)[a[@"rgbBlendOperation"] unsignedIntegerValue];
        c.sourceAlphaBlendFactor=(MTLBlendFactor)[a[@"sourceAlphaBlendFactor"] unsignedIntegerValue];
        c.destinationAlphaBlendFactor=(MTLBlendFactor)[a[@"destinationAlphaBlendFactor"] unsignedIntegerValue];
        c.alphaBlendOperation=(MTLBlendOperation)[a[@"alphaBlendOperation"] unsignedIntegerValue];
    }
    return d;
}
static MTL4RenderPipelineDescriptor* capturedBase(MTLRenderPipelineDescriptor* old,id<MTLLibrary> v,id<MTLLibrary> f) {
    auto d=base(old,v,f);
    d.fragmentFunctionDescriptor=f ? function(f,old.fragmentFunction.name) : nil;
    d.alphaToCoverageState=old.alphaToCoverageEnabled ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
    d.alphaToOneState=old.alphaToOneEnabled ? MTL4AlphaToOneStateEnabled : MTL4AlphaToOneStateDisabled;
    d.supportIndirectCommandBuffers=old.supportIndirectCommandBuffers ? MTL4IndirectCommandBufferSupportStateEnabled : MTL4IndirectCommandBufferSupportStateDisabled;
    MTL4PipelineOptions* options=[MTL4PipelineOptions new]; options.shaderValidation=old.shaderValidation; d.options=options;
    return d;
}
int main(int argc,const char* argv[]) {
    @autoreleasepool {
        if(argc!=4)return 64;
        alarm(180);
        NSString* mode=@(argv[1]); if(![mode isEqual:@"legacy"] && ![mode isEqual:@"metal4"])return 64;
        NSArray* inputs=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[2])] options:0 error:nil];
        if(!inputs.count)return 64;
        id<MTLDevice> device=MTLCreateSystemDefaultDevice(); if(!device)return 69;
        NSError* error=nil; id<MTL4Compiler> compiler=[device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
        NSMutableDictionary* libraries=[NSMutableDictionary new];
        NSMutableDictionary* bases=[NSMutableDictionary new]; NSMutableArray* lru=[NSMutableArray new];
        NSMutableArray* rows=[NSMutableArray new]; NSMutableArray* loads=[NSMutableArray new];
        char cacheDirectory[4096]={};confstr(_CS_DARWIN_USER_CACHE_DIR,cacheDirectory,sizeof(cacheDirectory));
        NSMutableDictionary* result=[@{@"schemaVersion":@2,@"mode":mode,@"device":device.name,@"rows":rows,@"loads":loads,
            @"os":[NSProcessInfo processInfo].operatingSystemVersionString,
            @"darwinUserCacheDirectory":@(cacheDirectory),@"bundleIdentifier":[NSBundle mainBundle].bundleIdentifier ?: @"unbundled",
            @"descriptorScope":@"exact captured Metal descriptors and native base keys; no draw/dispatch",
            @"libraryPolicy":@"retain all cohort libraries equally in A/B; not a product IR512 lifetime test",
            @"basePolicy":@"1024 entry LRU; not the product service cost-based admission policy",
            @"footprintBeforeBytes":footprint() ?: (id)[NSNull null]} mutableCopy];
        bool failed=false; NSUInteger evictions=0;
        for(NSUInteger repeat=0;repeat<2;repeat++) {
            for(NSDictionary* input in inputs) {
                @autoreleasepool {
                    auto getLibrary=^id<MTLLibrary>(NSString* path) {
                        if(!path || (id)path==[NSNull null])return (id<MTLLibrary>)nil;
                        id<MTLLibrary> value=libraries[path];
                        if(!value) { value=load(device,path,loads); if(value)libraries[path]=value; }
                        return value;
                    };
                    bool compute=[input[@"kind"] isEqual:@"compute"];
                    NSMutableDictionary* row=[@{@"input":input[@"label"],@"kind":compute ? @"compute":@"render",@"repeat":@(repeat),@"baseNs":@0} mutableCopy];
                    id pipeline=nil; error=nil;
                    if(compute) {
                        id<MTLLibrary> c=getLibrary(input[@"compute"]); MTLComputePipelineDescriptor* d=[MTLComputePipelineDescriptor new];
                        d.computeFunction=[c newFunctionWithName:input[@"computeEntry"]]; NSDictionary* s=input[@"state"];
                        d.maxTotalThreadsPerThreadgroup=[s[@"maxTotalThreadsPerThreadgroup"] unsignedIntegerValue];
                        d.threadGroupSizeIsMultipleOfThreadExecutionWidth=[s[@"threadGroupSizeIsMultipleOfThreadExecutionWidth"] boolValue];
                        NSArray* group=s[@"requiredThreadsPerThreadgroup"];
                        d.requiredThreadsPerThreadgroup=MTLSizeMake([group[0] unsignedIntegerValue],[group[1] unsignedIntegerValue],[group[2] unsignedIntegerValue]);
                        d.supportIndirectCommandBuffers=[s[@"supportIndirectCommandBuffers"] boolValue];
                        d.shaderValidation=(MTLShaderValidation)[s[@"shaderValidation"] unsignedIntegerValue];
                        uint64_t start=ns();
                        if([mode isEqual:@"legacy"]) {
                            pipeline=[device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
                        } else {
                            MTL4ComputePipelineDescriptor* next=[MTL4ComputePipelineDescriptor new];
                            next.computeFunctionDescriptor=function(c,d.computeFunction.name);
                            next.maxTotalThreadsPerThreadgroup=d.maxTotalThreadsPerThreadgroup;
                            next.requiredThreadsPerThreadgroup=d.requiredThreadsPerThreadgroup;
                            next.threadGroupSizeIsMultipleOfThreadExecutionWidth=d.threadGroupSizeIsMultipleOfThreadExecutionWidth;
                            next.supportIndirectCommandBuffers=d.supportIndirectCommandBuffers ? MTL4IndirectCommandBufferSupportStateEnabled : MTL4IndirectCommandBufferSupportStateDisabled;
                            MTL4PipelineOptions* options=[MTL4PipelineOptions new];options.shaderValidation=d.shaderValidation;next.options=options;
                            pipeline=[compiler newComputePipelineStateWithDescriptor:next compilerTaskOptions:nil error:&error];
                        }
                        row[@"totalNs"]=@(ns()-start);
                    } else {
                        id<MTLLibrary> v=getLibrary(input[@"vertex"]),f=getLibrary(input[@"fragment"]);
                        MTLRenderPipelineDescriptor* old=restoreRender(input,v,f);
                        uint64_t start=ns();
                        if([mode isEqual:@"legacy"]) {
                            pipeline=[device newRenderPipelineStateWithDescriptor:old error:&error];row[@"legacyNs"]=@(ns()-start);
                        } else {
                            NSString* key=input[@"baseKeyBase64"]; id<MTLRenderPipelineState> bp=bases[key];
                            if(!bp) {
                                bp=[compiler newRenderPipelineStateWithDescriptor:capturedBase(old,v,f) compilerTaskOptions:nil error:&error];
                                row[@"baseNs"]=@(ns()-start);
                                if(bp) {
                                    if(bases.count==1024) { [bases removeObjectForKey:lru.firstObject];[lru removeObjectAtIndex:0];evictions++; }
                                    bases[key]=bp;
                                } else row[@"baseError"]=failure(error);
                            }
                            if(bp) {
                                [lru removeObject:key];[lru addObject:key];
                                MTL4RenderPipelineDescriptor* next=(MTL4RenderPipelineDescriptor*)[bp newRenderPipelineDescriptorForSpecialization];
                                specialize(next,old);uint64_t specialized=ns();
                                pipeline=[compiler newRenderPipelineStateBySpecializationWithDescriptor:next pipeline:bp error:&error];
                                row[@"specializationNs"]=@(ns()-specialized);
                            }
                        }
                        row[@"totalNs"]=@(ns()-start);
                    }
                    row[@"success"]=@(pipeline!=nil);row[@"footprintBytes"]=footprint() ?: (id)[NSNull null];
                    if(!pipeline) { failed=true;row[@"error"]=failure(error); }
                    [rows addObject:row];
                }
            }
        }
        result[@"success"]=@(!failed);result[@"baseEvictions"]=@(evictions);
        result[@"cohortLibraries"]=@(libraries.count);result[@"residentBases"]=@(bases.count);
        result[@"footprintAfterBytes"]=footprint() ?: (id)[NSNull null];
        NSData* data=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
        if(![data writeToFile:@(argv[3]) options:NSDataWritingWithoutOverwriting error:nil])return 73;
        printf("mode=%s rows=%lu success=%s\n",argv[1],(unsigned long)rows.count,failed ? "false":"true");
        return failed ? 1:0;
    }
}
