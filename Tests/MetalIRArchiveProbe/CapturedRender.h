// Legacy descriptor restoration reused from task 3; no Metal 4 dependency.
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
