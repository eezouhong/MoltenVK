#include "vulkan_context.h"
#include <dlfcn.h>

int main(int argc,char** argv) {
    if (argc!=3 && argc!=4) return 64;
    auto enable=reinterpret_cast<uint32_t(*)(uint32_t)>(dlsym(RTLD_DEFAULT,"vkSetMetalIRTelemetryEnabledMVK"));
    auto stats=reinterpret_cast<uint32_t(*)(VkDevice,uint64_t*,uint32_t)>(dlsym(RTLD_DEFAULT,"vkGetMetalIRCompilerStatisticsMVK"));
    if (!enable||!stats||enable(1)==UINT32_MAX) return 1;
    Context context;
    context.target();
    if (argc==4 && std::string(argv[3])=="layout-only") {
        // Valid Vulkan dynamic UBO layout: unsupported by IR, before any shader attempt.
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo create{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        create.bindingCount=1;create.pBindings=&binding;
        VkDescriptorSetLayout layout{};
        VkResult result=vkCreateDescriptorSetLayout(context.device,&create,nullptr,&layout);
        uint64_t values[9]{};
        bool valid=result==VK_ERROR_FEATURE_NOT_PRESENT && layout==VK_NULL_HANDLE && stats(context.device,values,9)==9;
        printf("{\"layoutRejected\":%s,\"counter\":%llu,\"result\":%d}\n",valid?"true":"false",(unsigned long long)values[7],result);
        return valid ? 0 : 1;
    }
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[0].module=context.shader(argv[1]);stages[1].module=context.shader(argv[2]);
    stages[0].pName=stages[1].pName="main";
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};input.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,16,16,0,1};VkRect2D rect{{0,0},{16,16}};
    VkPipelineViewportStateCreateInfo view{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};view.viewportCount=1;view.pViewports=&viewport;view.scissorCount=1;view.pScissors=&rect;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.rasterizerDiscardEnable=VK_TRUE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState color{};color.colorWriteMask=0xf;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&color;
    VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};create.stageCount=2;create.pStages=stages;
    create.pVertexInputState=&vertex;create.pInputAssemblyState=&input;create.pViewportState=&view;create.pRasterizationState=&raster;create.pMultisampleState=&multisample;create.pColorBlendState=&blend;create.layout=context.layout;create.renderPass=context.renderPass;
    VkPipeline pipeline{};VkResult result=vkCreateGraphicsPipelines(context.device,VK_NULL_HANDLE,1,&create,nullptr,&pipeline);
    uint64_t values[9]{};bool counted=stats(context.device,values,9)==9&&values[7]==1;
    if(result!=VK_ERROR_FEATURE_NOT_PRESENT||pipeline!=VK_NULL_HANDLE||!counted)return 1;
    printf("{\"rejected\":true,\"nullHandle\":true,\"counter\":1,\"result\":%d}\n",result);
}
