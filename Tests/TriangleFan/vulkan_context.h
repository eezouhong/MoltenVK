#pragma once
// Local Vulkan execution fixtures. Expected system values come from the Vulkan
// command arguments, independently of either compiler's output.
#include <vulkan/vulkan.h>
#include <array>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

#define VK_CHECK(call) do { auto vkCheckedResult = (call); if (vkCheckedResult != VK_SUCCESS) \
    throw std::runtime_error(std::string(#call) + ": " + std::to_string(vkCheckedResult)); } while (0)

struct Buffer { VkBuffer handle{}; VkDeviceMemory memory{}; void* mapped{}; };
using Row = std::array<uint32_t, 4>;

class Context {
public:
    VkInstance instance{};
    VkDevice device{};
    VkQueue queue{};
    VkPhysicalDevice physical{};
    uint32_t family{};
    bool negativeDepth = false;
    bool fragmentStorage = false;
    VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout descriptorLayout{}, emptyLayout{}, zeroCountLayout{};
    VkDescriptorPool descriptorPool{};
    VkDescriptorSet descriptor{};
    VkPipelineLayout layout{};
    VkCommandPool commandPool{};
    VkCommandBuffer command{};
    VkRenderPass renderPass{};
    VkFramebuffer framebuffer{};
    VkImage image{};
    VkDeviceMemory imageMemory{};
    VkImageView view{};
    std::vector<Buffer> buffers;
    std::vector<VkShaderModule> modules;
    std::vector<VkPipeline> pipelines;

    explicit Context(bool emptySets = false, bool negativeDepthMode = false, bool largePoints = false,
                     bool floatControls2 = false, bool fragmentStorageMode = false)
        : negativeDepth(negativeDepthMode), fragmentStorage(fragmentStorageMode) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = floatControls2 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; info.pApplicationInfo = &app;
        VK_CHECK(vkCreateInstance(&info, nullptr, &instance));
        uint32_t count{}; VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        if (!count) throw std::runtime_error("no GPU");
        std::vector<VkPhysicalDevice> devices(count); VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        physical = devices[0];
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count); vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        family = UINT32_MAX;
        for (uint32_t i=0; i<count; ++i) if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) { family=i; break; }
        if (family == UINT32_MAX) throw std::runtime_error("no combined queue");
        float priority=1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkPhysicalDeviceShaderDrawParametersFeatures draw{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES};
        draw.shaderDrawParameters=VK_TRUE;
        VkPhysicalDeviceFeatures features{}; features.vertexPipelineStoresAndAtomics=VK_TRUE;
        features.fragmentStoresAndAtomics=fragmentStorage;
        features.largePoints=largePoints;
        features.multiDrawIndirect=VK_TRUE; features.drawIndirectFirstInstance=VK_TRUE;
        const char* extensions[]={"VK_KHR_portability_subset", floatControls2 ? VK_KHR_SHADER_FLOAT_CONTROLS_2_EXTENSION_NAME : VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
                                  VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME};
        VkPhysicalDeviceDepthClipControlFeaturesEXT depth{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT}; depth.depthClipControl=VK_TRUE;
        VkPhysicalDeviceShaderFloatControls2FeaturesKHR math{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT_CONTROLS_2_FEATURES_KHR}; math.shaderFloatControls2=VK_TRUE;
        if(floatControls2) { draw.pNext=&math; if(negativeDepth) math.pNext=&depth; }
        else if(negativeDepth) draw.pNext=&depth;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.pNext=&draw; dc.pEnabledFeatures=&features;
        dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qi; dc.enabledExtensionCount=1+unsigned(floatControls2)+unsigned(negativeDepth); dc.ppEnabledExtensionNames=extensions;
        VK_CHECK(vkCreateDevice(physical, &dc, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        VkShaderStageFlags outputStages=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT;
        if(fragmentStorage)outputStages|=VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,outputStages,nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=1; dl.pBindings=&binding;
        VK_CHECK(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptorLayout));
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=1; dp.pPoolSizes=&poolSize;
        VK_CHECK(vkCreateDescriptorPool(device,&dp,nullptr,&descriptorPool));
        VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ds.descriptorPool=descriptorPool; ds.descriptorSetCount=1; ds.pSetLayouts=&descriptorLayout;
        VK_CHECK(vkAllocateDescriptorSets(device,&ds,&descriptor));
        VkPushConstantRange push{outputStages,0,32};
        VkDescriptorSetLayoutCreateInfo empty{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (emptySets) {
            VK_CHECK(vkCreateDescriptorSetLayout(device,&empty,nullptr,&emptyLayout));
            VkDescriptorSetLayoutBinding zero{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,0,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            empty.bindingCount=1; empty.pBindings=&zero;
            VK_CHECK(vkCreateDescriptorSetLayout(device,&empty,nullptr,&zeroCountLayout));
        }
        VkDescriptorSetLayout layouts[]={descriptorLayout,emptyLayout,zeroCountLayout,emptyLayout};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=emptySets?4:1; pl.pSetLayouts=layouts; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&push;
        VK_CHECK(vkCreatePipelineLayout(device,&pl,nullptr,&layout));
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cp.queueFamilyIndex=family;
        VK_CHECK(vkCreateCommandPool(device,&cp,nullptr,&commandPool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ca.commandPool=commandPool; ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount=1;
        VK_CHECK(vkAllocateCommandBuffers(device,&ca,&command));
    }

    uint32_t memoryType(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags) {
        for(uint32_t i=0;i<memory.memoryTypeCount;++i)
            if((requirements.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("required memory type unavailable");
    }
    VkDeviceMemory allocate(VkMemoryRequirements requirements, VkMemoryPropertyFlags flags) {
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=requirements.size; ai.memoryTypeIndex=memoryType(requirements,flags);
        VkDeviceMemory allocation; VK_CHECK(vkAllocateMemory(device,&ai,nullptr,&allocation)); return allocation;
    }
    Buffer buffer(size_t bytes,VkBufferUsageFlags usage) {
        Buffer result{};
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=bytes; bi.usage=usage; bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(device,&bi,nullptr,&result.handle));
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device,result.handle,&requirements);
        result.memory=allocate(requirements,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkBindBufferMemory(device,result.handle,result.memory,0)); VK_CHECK(vkMapMemory(device,result.memory,0,bytes,0,&result.mapped));
        memset(result.mapped,0xcd,bytes); buffers.push_back(result); return result;
    }
    VkShaderModule shader(const char* path) {
        std::ifstream file(path,std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
        if(bytes.empty()||bytes.size()%4) throw std::runtime_error("invalid shader fixture");
        std::vector<uint32_t> words(bytes.size()/4); memcpy(words.data(),bytes.data(),bytes.size());
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=bytes.size(); info.pCode=words.data();
        VkShaderModule result; VK_CHECK(vkCreateShaderModule(device,&info,nullptr,&result)); modules.push_back(result); return result;
    }
    void output(Buffer buffer,size_t bytes) {
        VkDescriptorBufferInfo info{buffer.handle,0,bytes}; VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=descriptor; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; write.pBufferInfo=&info;
        vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    }
    void begin() { VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; VK_CHECK(vkBeginCommandBuffer(command,&info)); }
    void barrier(VkPipelineStageFlags source,VkAccessFlags sourceAccess,VkPipelineStageFlags destination,VkAccessFlags destinationAccess) {
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=sourceAccess; barrier.dstAccessMask=destinationAccess;
        vkCmdPipelineBarrier(command,source,destination,0,1,&barrier,0,nullptr,0,nullptr);
    }
    void finish() {
        VkPipelineStageFlags outputStages=VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        if(fragmentStorage)outputStages|=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier(outputStages,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
        VK_CHECK(vkEndCommandBuffer(command));
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence; VK_CHECK(vkCreateFence(device,&fi,nullptr,&fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&command;
        VK_CHECK(vkQueueSubmit(queue,1,&submit,fence)); VK_CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,15ull*1000000000));
        vkDestroyFence(device,fence,nullptr);
    }
    void target(bool readback = false) {
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D; ii.format=VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent={16,16,1}; ii.mipLevels=ii.arrayLayers=1; ii.samples=VK_SAMPLE_COUNT_1_BIT; ii.tiling=VK_IMAGE_TILING_OPTIMAL; ii.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|(readback?VK_IMAGE_USAGE_TRANSFER_SRC_BIT:0);
        VK_CHECK(vkCreateImage(device,&ii,nullptr,&image)); VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device,image,&requirements);
        imageMemory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); VK_CHECK(vkBindImageMemory(device,image,imageMemory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=ii.format; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VK_CHECK(vkCreateImageView(device,&vi,nullptr,&view));
        VkAttachmentDescription attachment{}; attachment.format=ii.format; attachment.samples=ii.samples; attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=readback?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}; VkSubpassDescription subpass{}; subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount=1; subpass.pColorAttachments=&reference;
        VkSubpassDependency dependency{VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_SHADER_READ_BIT,0};
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount=1; rp.pAttachments=&attachment; rp.subpassCount=1; rp.pSubpasses=&subpass; rp.dependencyCount=1; rp.pDependencies=&dependency;
        VK_CHECK(vkCreateRenderPass(device,&rp,nullptr,&renderPass));
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fb.renderPass=renderPass; fb.attachmentCount=1; fb.pAttachments=&view; fb.width=fb.height=16; fb.layers=1;
        VK_CHECK(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer));
    }
    VkPipeline graphics(VkShaderModule vertex,VkShaderModule fragment,VkPrimitiveTopology topology, bool threeInputs=false) {
        VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vertex; stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=fragment;
        for(auto& stage:stages) stage.pName="main";
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkVertexInputBindingDescription binding{0,48,VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[]={{0,0,VK_FORMAT_R32G32B32A32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32A32_SFLOAT,16},{2,0,VK_FORMAT_R32G32B32A32_SFLOAT,32}};
        if (threeInputs) { input.vertexBindingDescriptionCount=1; input.pVertexBindingDescriptions=&binding; input.vertexAttributeDescriptionCount=3; input.pVertexAttributeDescriptions=attributes; }
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=topology; assembly.primitiveRestartEnable=topology==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        VkViewport viewport{0,0,16,16,0,1}; VkRect2D scissor{{0,0},{16,16}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=vp.scissorCount=1; vp.pViewports=&viewport; vp.pScissors=&scissor;
        VkPipelineViewportDepthClipControlCreateInfoEXT depth{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT}; depth.negativeOneToOne=VK_TRUE;
        if(negativeDepth) vp.pNext=&depth;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=1; blend.pAttachments=&attachment;
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; info.stageCount=2; info.pStages=stages; info.pVertexInputState=&input; info.pInputAssemblyState=&assembly;
        info.pViewportState=&vp; info.pRasterizationState=&raster; info.pMultisampleState=&ms; info.pColorBlendState=&blend; info.layout=layout; info.renderPass=renderPass;
        VkPipeline result; VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&result)); pipelines.push_back(result); return result;
    }
    VkPipeline compute(VkShaderModule module) {
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; info.flags=VK_PIPELINE_CREATE_DISPATCH_BASE_BIT; info.layout=layout;
        info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}; info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT; info.stage.module=module; info.stage.pName="main";
        VkPipeline result; VK_CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&result)); pipelines.push_back(result); return result;
    }
    ~Context() {
        if(device) {
            vkDeviceWaitIdle(device);
            vkDestroyCommandPool(device,commandPool,nullptr);
            for(auto p:pipelines) vkDestroyPipeline(device,p,nullptr);
            for(auto m:modules) vkDestroyShaderModule(device,m,nullptr);
            vkDestroyFramebuffer(device,framebuffer,nullptr); vkDestroyRenderPass(device,renderPass,nullptr);
            vkDestroyImageView(device,view,nullptr); vkDestroyImage(device,image,nullptr); vkFreeMemory(device,imageMemory,nullptr);
            vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyDescriptorPool(device,descriptorPool,nullptr); vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr); vkDestroyDescriptorSetLayout(device,emptyLayout,nullptr); vkDestroyDescriptorSetLayout(device,zeroCountLayout,nullptr);
            for(auto b:buffers) { vkUnmapMemory(device,b.memory); vkDestroyBuffer(device,b.handle,nullptr); vkFreeMemory(device,b.memory,nullptr); }
            vkDestroyDevice(device,nullptr);
        }
        if(instance) vkDestroyInstance(instance,nullptr);
    }
};

inline bool checkRows(Buffer output,const std::vector<Row>& expected,const char* name) {
    auto* actual=static_cast<Row*>(output.mapped); uint32_t correct=0;
    for(size_t i=0;i<expected.size();++i) {
        if(actual[i]==expected[i]) ++correct;
        else if(i<expected.size() && correct==i) fprintf(stderr,"first mismatch %s row %zu: got %u,%u,%u,%u expected %u,%u,%u,%u\n",name,i,
            actual[i][0],actual[i][1],actual[i][2],actual[i][3],expected[i][0],expected[i][1],expected[i][2],expected[i][3]);
    }
    printf("{\"case\":\"%s\",\"correct\":%u,\"total\":%zu,\"success\":%s}\n",name,correct,expected.size(),correct==expected.size()?"true":"false");
    return correct==expected.size();
}
