#include "vulkan_context.h"
#include <dlfcn.h>
int main(int argc,char** argv) {
    if(argc!=4)return 64;
    alarm(60);
    try {
        bool trace=std::string(argv[1])=="on";
        Context context(false,false,false,false,false,true);context.target();
        Buffer output=context.buffer(4*sizeof(Row),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        context.output(output,4*sizeof(Row));
        VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage=VK_SHADER_STAGE_MESH_BIT_EXT;stages[0].module=context.shader(argv[2]);stages[0].pName="main";
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=context.shader(argv[3]);stages[1].pName="main";
        VkViewport viewport{0,0,16,16,0,1};VkRect2D scissor{{0,0},{16,16}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};ci.stageCount=2;ci.pStages=stages;ci.pViewportState=&vp;ci.pRasterizationState=&raster;ci.pMultisampleState=&ms;ci.pColorBlendState=&blend;ci.layout=context.layout;ci.renderPass=context.renderPass;
        VkPipeline pipeline{};VK_CHECK(vkCreateGraphicsPipelines(context.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline));context.pipelines.push_back(pipeline);
        auto draw=reinterpret_cast<PFN_vkCmdDrawMeshTasksIndirectEXT>(vkGetDeviceProcAddr(context.device,"vkCmdDrawMeshTasksIndirectEXT"));
        if(!draw)throw std::runtime_error("mesh indirect entry unavailable");
        VkDrawMeshTasksIndirectCommandEXT args[2]={{1,1,1},{2,1,1}};
        Buffer indirect=context.buffer(sizeof(args),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);memcpy(indirect.mapped,args,sizeof(args));
        context.begin();VkClearValue clear{};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=context.renderPass;begin.framebuffer=context.framebuffer;begin.renderArea=scissor;begin.clearValueCount=1;begin.pClearValues=&clear;
        vkCmdBeginRenderPass(context.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,context.layout,0,1,&context.descriptor,0,nullptr);
        draw(context.command,indirect.handle,0,2,sizeof(args[0]));
        vkCmdEndRenderPass(context.command);context.finish();
        std::vector<Row> expected(4,Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});expected[0]=expected[1]={31,41,59,26};
        if(!checkRows(output,expected,"mesh-indirect-gpu-output"))return 1;
        auto finish=reinterpret_cast<uint64_t(*)()>(dlsym(RTLD_DEFAULT,"vkFinishReplayFrameMVK"));
        if(trace ? !finish||finish()!=1 : finish!=nullptr)return 2;
        printf("{\"meshIndirectDraws\":2,\"traceEnabled\":%s,\"success\":true}\n",trace?"true":"false");
        return 0;
    }catch(const std::exception& error){fprintf(stderr,"mesh trace probe: %s\n",error.what());return 1;}
}
