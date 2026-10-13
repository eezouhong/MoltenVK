// Independent CPU oracle for attachment conversion, blend and component masks.
// No captured shaders. Keep this fixture independent of the shared Context API.
#include "vulkan_context.h"
#include <algorithm>
#include <cmath>

struct FormatContext : Context {
    void formatTarget(VkFormat format, bool blending) {
        VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(physical,format,&properties);
        VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
        if(blending) required|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if((properties.optimalTilingFeatures&required)!=required) throw std::runtime_error("fixture format unsupported");
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D;ii.format=format;
        ii.extent={16,16,1};ii.mipLevels=ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;
        ii.tiling=VK_IMAGE_TILING_OPTIMAL;ii.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateImage(device,&ii,nullptr,&image));VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device,image,&requirements);imageMemory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkBindImageMemory(device,image,imageMemory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        vi.format=format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VK_CHECK(vkCreateImageView(device,&vi,nullptr,&view));
        VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=ii.samples;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference reference{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount=1;subpass.pColorAttachments=&reference;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;
        rp.subpassCount=1;rp.pSubpasses=&subpass;VK_CHECK(vkCreateRenderPass(device,&rp,nullptr,&renderPass));
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=renderPass;
        fb.attachmentCount=1;fb.pAttachments=&view;fb.width=fb.height=16;fb.layers=1;
        VK_CHECK(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer));
    }
    VkPipeline formatPipeline(VkShaderModule vs,VkShaderModule fs,bool blending,unsigned mask) {
        VkPipelineShaderStageCreateInfo stages[2]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vs;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=fs;
        for(auto& s:stages)s.pName="main";
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0,0,16,16,0,1};VkRect2D scissor{{0,0},{16,16}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=vp.scissorCount=1;vp.pViewports=&viewport;vp.pScissors=&scissor;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=mask;attachment.blendEnable=blending;
        attachment.srcColorBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;attachment.dstColorBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;attachment.colorBlendOp=VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;attachment.alphaBlendOp=VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.stageCount=2;pi.pStages=stages;
        pi.pVertexInputState=&input;pi.pInputAssemblyState=&assembly;pi.pViewportState=&vp;pi.pRasterizationState=&raster;
        pi.pMultisampleState=&ms;pi.pColorBlendState=&blend;pi.layout=layout;pi.renderPass=renderPass;
        VkPipeline p{};VK_CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&p));pipelines.push_back(p);return p;
    }
};

static double encode(double x) { return x<=.0031308?12.92*x:1.055*std::pow(x,1.0/2.4)-.055; }
static double decode(double x) { return x<=.04045?x/12.92:std::pow((x+.055)/1.055,2.4); }
static unsigned quantize(double x) { return unsigned(std::lround(std::clamp(x,0.0,1.0)*255.0)); }

int main(int argc,char** argv) {try {
    if(argc!=5)return 64;
    const std::string kind=argv[1];
    const bool integer=kind=="uint"||kind=="uint-mask";
    const bool srgb=kind=="srgb"||kind=="srgb-blend";
    const bool blending=kind=="unorm-blend"||kind=="srgb-blend";
    const unsigned mask=kind=="unorm-mask"||kind=="uint-mask"?5:kind=="unorm-no-write"?0:15;
    if(!integer&&!srgb&&kind!="unorm"&&kind!="unorm-blend"&&kind!="unorm-mask"&&kind!="unorm-no-write")return 64;
    alarm(60);
    FormatContext c;c.formatTarget(integer?VK_FORMAT_R8G8B8A8_UINT:srgb?VK_FORMAT_R8G8B8A8_SRGB:VK_FORMAT_R8G8B8A8_UNORM,blending);
    auto pipeline=c.formatPipeline(c.shader(argv[2]),c.shader(integer?argv[4]:argv[3]),blending,mask);
    Buffer pixels=c.buffer(16*16*4+16,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkClearValue clear{};const double dst[4]={.1,.2,.3,.4},src[4]={.25,.5,.75,.5};
    for(unsigned i=0;i<4;++i)if(integer)clear.color.uint32[i]=i+1;else clear.color.float32[i]=float(dst[i]);
    c.begin();VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};pass.renderPass=c.renderPass;pass.framebuffer=c.framebuffer;
    pass.renderArea={{0,0},{16,16}};pass.clearValueCount=1;pass.pClearValues=&clear;
    vkCmdBeginRenderPass(c.command,&pass,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdDraw(c.command,3,1,0,0);vkCmdEndRenderPass(c.command);
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.image=c.image;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={16,16,1};
    vkCmdCopyImageToBuffer(c.command,c.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,pixels.handle,1,&copy);
    c.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);c.finish();
    const unsigned uintSrc[4]={17,93,201,7};unsigned expected[4]{};
    for(unsigned i=0;i<4;++i) {
        if(integer) {expected[i]=(mask&(1u<<i))?uintSrc[i]:i+1;continue;}
        const bool nonlinear=srgb&&i<3;
        const unsigned clearByte=quantize(nonlinear?encode(dst[i]):dst[i]);
        if(!(mask&(1u<<i))) {expected[i]=clearByte;continue;}
        double value=src[i];
        if(blending) {
            const double destination=nonlinear?decode(clearByte/255.0):clearByte/255.0;
            value=src[i]*(i==3?1.0:src[3])+destination*(1.0-src[3]);
        }
        expected[i]=quantize(nonlinear?encode(value):value);
    }
    auto* actual=static_cast<uint8_t*>(pixels.mapped);unsigned correct=0,maxDiff=0;
    for(unsigned p=0;p<256;++p) {
        bool valid=true;for(unsigned i=0;i<4;++i) {unsigned d=unsigned(std::abs(int(actual[p*4+i])-int(expected[i])));maxDiff=std::max(maxDiff,d);valid&=d<=(integer?0u:1u);}
        correct+=valid;
        if(!valid&&p<4)fprintf(stderr,"pixel %u got %u,%u,%u,%u expected %u,%u,%u,%u\n",p,actual[p*4],actual[p*4+1],actual[p*4+2],actual[p*4+3],expected[0],expected[1],expected[2],expected[3]);
    }
    bool guard=true;for(unsigned i=1024;i<1040;++i)guard&=actual[i]==0xcd;
    bool success=correct==256&&guard;
    printf("{\"case\":\"%s\",\"correctPixels\":%u,\"totalPixels\":256,\"maxDiff\":%u,\"guard\":%s,\"expected\":[%u,%u,%u,%u],\"success\":%s}\n",kind.c_str(),correct,maxDiff,guard?"true":"false",expected[0],expected[1],expected[2],expected[3],success?"true":"false");
    return success?0:1;
}catch(const std::exception& e){fprintf(stderr,"formats fixture: %s\n",e.what());return 1;}}
