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
    VkPhysicalDeviceMemoryProperties memory{};
    VkDescriptorSetLayout descriptorLayout{}, emptyLayout{};
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

    explicit Context(bool emptySets = false, bool negativeDepthMode = false, bool largePoints = false) : negativeDepth(negativeDepthMode) {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
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
        features.largePoints=largePoints;
        features.multiDrawIndirect=VK_TRUE; features.drawIndirectFirstInstance=VK_TRUE;
        const char* extensions[]={"VK_KHR_portability_subset",VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME};
        VkPhysicalDeviceDepthClipControlFeaturesEXT depth{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT}; depth.depthClipControl=VK_TRUE;
        if(negativeDepth) draw.pNext=&depth;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.pNext=&draw; dc.pEnabledFeatures=&features;
        dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qi; dc.enabledExtensionCount=negativeDepth?2:1; dc.ppEnabledExtensionNames=extensions;
        VK_CHECK(vkCreateDevice(physical, &dc, nullptr, &device)); vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=1; dl.pBindings=&binding;
        VK_CHECK(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptorLayout));
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=1; dp.pPoolSizes=&poolSize;
        VK_CHECK(vkCreateDescriptorPool(device,&dp,nullptr,&descriptorPool));
        VkDescriptorSetAllocateInfo ds{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ds.descriptorPool=descriptorPool; ds.descriptorSetCount=1; ds.pSetLayouts=&descriptorLayout;
        VK_CHECK(vkAllocateDescriptorSets(device,&ds,&descriptor));
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,32};
        VkDescriptorSetLayoutCreateInfo empty{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        if (emptySets) VK_CHECK(vkCreateDescriptorSetLayout(device,&empty,nullptr,&emptyLayout));
        VkDescriptorSetLayout layouts[]={descriptorLayout,emptyLayout,emptyLayout,emptyLayout};
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
        barrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
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
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=topology;
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
            vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyDescriptorPool(device,descriptorPool,nullptr); vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr); vkDestroyDescriptorSetLayout(device,emptyLayout,nullptr);
            for(auto b:buffers) { vkUnmapMemory(device,b.memory); vkDestroyBuffer(device,b.handle,nullptr); vkFreeMemory(device,b.memory,nullptr); }
            vkDestroyDevice(device,nullptr);
        }
        if(instance) vkDestroyInstance(instance,nullptr);
    }
};

static bool checkRows(Buffer output,const std::vector<Row>& expected,const char* name) {
    auto* actual=static_cast<Row*>(output.mapped); uint32_t correct=0;
    for(size_t i=0;i<expected.size();++i) {
        if(actual[i]==expected[i]) ++correct;
        else if(i<expected.size() && correct==i) fprintf(stderr,"first mismatch %s row %zu: got %u,%u,%u,%u expected %u,%u,%u,%u\n",name,i,
            actual[i][0],actual[i][1],actual[i][2],actual[i][3],expected[i][0],expected[i][1],expected[i][2],expected[i][3]);
    }
    printf("{\"case\":\"%s\",\"correct\":%u,\"total\":%zu,\"success\":%s}\n",name,correct,expected.size(),correct==expected.size()?"true":"false");
    return correct==expected.size();
}

static bool graphics(const char* vertex,const char* fragment,bool fans) {
    Context context; context.target();
    const uint32_t total=512;
    Buffer output=context.buffer(total*sizeof(Row),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); context.output(output,total*sizeof(Row));
    Buffer indices=context.buffer(256,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    Buffer indices16=context.buffer(128,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    auto* i32=reinterpret_cast<uint32_t*>(static_cast<char*>(indices.mapped)+16);
    auto* i16=reinterpret_cast<uint16_t*>(static_cast<char*>(indices16.mapped)+8);
    for(uint32_t i=0;i<3;++i) { i32[2+i]=7+i; i32[8+i]=20+i; i16[2+i]=20+i; }
    Buffer indirect=context.buffer(256,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto pipeline=context.graphics(context.shader(vertex),context.shader(fragment), fans?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    std::vector<Row> expected(total,Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});
    context.begin();
    uint32_t commands[16]{};
    if(!fans) {
        VkDrawIndirectCommand first{3,2,10,3},second{3,2,20,7};
        memcpy(commands,&first,sizeof(first)); memcpy(commands+8,&second,sizeof(second));
        vkCmdUpdateBuffer(context.command,indirect.handle,32,sizeof(commands),commands);
        VkDrawIndexedIndirectCommand indexedFirst{3,2,2,-4,5},indexedSecond{3,2,8,6,7};
        memcpy(commands,&indexedFirst,sizeof(indexedFirst)); memcpy(commands+8,&indexedSecond,sizeof(indexedSecond));
        vkCmdUpdateBuffer(context.command,indirect.handle,128,sizeof(commands),commands);
        context.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    }
    VkClearValue clear{}; VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass=context.renderPass; begin.framebuffer=context.framebuffer;
    begin.renderArea={{0,0},{16,16}}; begin.clearValueCount=1; begin.pClearValues=&clear;
    vkCmdBeginRenderPass(context.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,context.layout,0,1,&context.descriptor,0,nullptr);
    auto prepare=[&](uint32_t destination, std::array<int32_t,2> origins, std::array<uint32_t,2> instances,std::array<int32_t,2> bases,uint32_t draws=1) {
        uint32_t push[8]={destination,0,static_cast<uint32_t>(origins[0]),static_cast<uint32_t>(origins[1]),instances[0],instances[1],0,0};
        vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        for(uint32_t d=0;d<draws;++d) for(uint32_t i=0;i<2;++i) for(uint32_t v=0;v<3;++v) {
            auto slot=destination+d*12+i*6+v*2;
            expected[slot]={static_cast<uint32_t>(origins[d])+v,instances[d]+i,static_cast<uint32_t>(bases[d]),instances[d]};
            expected[slot+1]={d,0xabcdef01,static_cast<uint32_t>(origins[d]),instances[d]};
        }
    };
    prepare(0,{7,0},{5,0},{7,0}); vkCmdDraw(context.command,3,2,7,5);
    prepare(32,{11,0},{2,0},{11,0}); vkCmdDraw(context.command,3,2,11,2);
    vkCmdBindIndexBuffer(context.command,indices.handle,16,VK_INDEX_TYPE_UINT32);
    prepare(64,{3,0},{9,0},{-4,0}); vkCmdDrawIndexed(context.command,3,2,2,-4,9);
    vkCmdBindIndexBuffer(context.command,indices16.handle,8,VK_INDEX_TYPE_UINT16);
    prepare(96,{12,0},{1,0},{-8,0}); vkCmdDrawIndexed(context.command,3,2,2,-8,1);
    if(!fans) {
        prepare(128,{10,20},{3,7},{10,20},2); vkCmdDrawIndirect(context.command,indirect.handle,32,2,32);
        vkCmdBindIndexBuffer(context.command,indices.handle,16,VK_INDEX_TYPE_UINT32);
        prepare(160,{3,26},{5,7},{-4,6},2); vkCmdDrawIndexedIndirect(context.command,indirect.handle,128,2,32);
    }
    vkCmdEndRenderPass(context.command); context.finish();
    return checkRows(output,expected,fans?"triangle-fan-system-values":"draw-system-values");
}

static bool compute(const char* runtime,const char* unused,bool emptySets=false) {
    Context context(emptySets);
    auto active=context.compute(context.shader(runtime)), inactive=context.compute(context.shader(unused));
    const uint32_t total=128;
    Buffer output=context.buffer(total*sizeof(Row),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); context.output(output,total*sizeof(Row));
    Buffer indirect=context.buffer(64,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::vector<Row> expected(total,Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});
    context.begin();
    uint32_t groups[]={2,2,2}; vkCmdUpdateBuffer(context.command,indirect.handle,16,sizeof(groups),groups);
    context.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
    auto prepare=[&](uint32_t destination,std::array<uint32_t,3> origin) {
        uint32_t push[8]={destination,0,0,0,origin[0],origin[1],origin[2],0};
        vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        for(uint32_t z=0;z<2;++z) for(uint32_t y=0;y<2;++y) for(uint32_t x=0;x<2;++x) {
            uint32_t slot=destination+((z*2+y)*2+x)*2;
            expected[slot]={origin[0]+x,origin[1]+y,origin[2]+z,2};
            expected[slot+1]={2,2,origin[0]+x,0xcafe};
        }
    };
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,active);
    prepare(0,{3,2,1}); vkCmdDispatchBase(context.command,3,2,1,2,2,2);
    prepare(32,{0,0,0}); vkCmdDispatch(context.command,2,2,2);
    prepare(64,{0,0,0}); vkCmdDispatchIndirect(context.command,indirect.handle,16);
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,inactive);
    uint32_t push[8]={96,0,0,0,0,0,0,0}; vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
    expected[96]={11,22,33,44}; vkCmdDispatch(context.command,1,1,1);
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,active);
    prepare(100,{5,1,3}); vkCmdDispatchBase(context.command,5,1,3,2,2,2);
    context.finish(); return checkRows(output,expected,"dispatch-system-values-and-switch");
}

int main(int argc,char** argv) {
    if(argc!=4) return 64;
    alarm(60);
    try {
        if (std::string(argv[1]) == "toggle") {
            setenv("MELONX_EXPERIMENTAL_METAL_IR", "0", 1);
            bool success = graphics(argv[2],argv[3],false);
            setenv("MELONX_EXPERIMENTAL_METAL_IR", "1", 1);
            success = graphics(argv[2],argv[3],false) && success;
            setenv("MELONX_EXPERIMENTAL_METAL_IR", "0", 1);
            success = graphics(argv[2],argv[3],false) && success;
            return success ? 0 : 1;
        }
        bool success=(std::string(argv[1])=="compute" || std::string(argv[1])=="empty")?compute(argv[2],argv[3],std::string(argv[1])=="empty"):graphics(argv[2],argv[3],std::string(argv[1])=="fan");
        return success?0:1;
    } catch(const std::exception& error) { fprintf(stderr,"probe failed: %s\n",error.what()); return 1; }
}
