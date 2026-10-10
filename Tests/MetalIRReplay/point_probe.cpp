#include "vulkan_context.h"
#include <cmath>
#include <dlfcn.h>

int main(int argc,char** argv) {
    if(argc!=4&&argc!=5)return 64;
    alarm(60);
    try {
        bool triangle=!strcmp(argv[3],"triangle");unsigned size=triangle?8:std::stoul(argv[3]);
        if(size!=1&&size!=2&&size!=4&&size!=8)return 64;
        Context context(false,false,true);context.target(true);
        if(argc==5) {
            auto configure=reinterpret_cast<VkResult(*)(VkDevice,const char*,uint64_t)>(dlsym(RTLD_DEFAULT,"vkConfigureMetalIRCacheMVK"));
            if(!configure)throw std::runtime_error("missing IR cache configuration");
            VK_CHECK(configure(context.device,argv[4],2ull*1024*1024*1024));
        }
        auto pipeline=context.graphics(context.shader(argv[1]),context.shader(argv[2]),
                                       triangle?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
        Buffer pixels=context.buffer(16*16*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        float center=size==1?8.5f:8.0f;
        struct Push { float x,y,size;uint32_t triangle;uint32_t reserved[4]; } push{center/8-1,center/8-1,float(size),uint32_t(triangle),{}};
        context.begin();VkClearValue clear{};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=context.renderPass;begin.framebuffer=context.framebuffer;
        begin.renderArea={{0,0},{16,16}};begin.clearValueCount=1;begin.pClearValues=&clear;
        vkCmdBeginRenderPass(context.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        vkCmdDraw(context.command,triangle?3:1,1,0,0);vkCmdEndRenderPass(context.command);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.image=context.image;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={16,16,1};
        vkCmdCopyImageToBuffer(context.command,context.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,pixels.handle,1,&copy);
        context.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
        context.finish();auto* actual=static_cast<uint8_t*>(pixels.mapped);unsigned correct=0,covered=0;
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x) {
            unsigned index=(y*16+x)*4;uint8_t expected[4]{};
            bool inside=triangle||(x+0.5f>center-size*0.5f&&x+0.5f<center+size*0.5f&&
                                   y+0.5f>center-size*0.5f&&y+0.5f<center+size*0.5f);
            if(inside) {
                ++covered;expected[3]=255;
                if(triangle)expected[2]=255;
                else {expected[0]=std::lround((0.5f+(x+0.5f-center)/size)*255);
                      expected[1]=std::lround((0.5f+(y+0.5f-center)/size)*255);expected[2]=64;}
            }
            bool valid=true;for(unsigned c=0;c<4;++c)valid&=std::abs(int(actual[index+c])-int(expected[c]))<=1;
            correct+=valid;
            if(!valid)fprintf(stderr,"pixel %u,%u got %u,%u,%u,%u expected %u,%u,%u,%u\n",x,y,
                actual[index],actual[index+1],actual[index+2],actual[index+3],expected[0],expected[1],expected[2],expected[3]);
        }
        printf("{\"triangle\":%s,\"pointSize\":%u,\"coveredPixels\":%u,\"correctPixels\":%u,\"totalPixels\":256,\"success\":%s}\n",
            triangle?"true":"false",size,covered,correct,correct==256?"true":"false");return correct==256?0:1;
    } catch(const std::exception& error) {fprintf(stderr,"point probe: %s\n",error.what());return 1;}
}
