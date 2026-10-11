#include "vulkan_context.h"
int main(int argc,char** argv) {
    if(argc!=4)return 64;
    alarm(60);
    try {
        bool viewport=std::string(argv[1])=="viewport";uint32_t layers=viewport?1:2;
        Context context(false,false,false,false,false,true);context.target(true,layers);
        Buffer pixels=context.buffer(16*16*4*layers,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        auto pipeline=context.graphics(context.shader(argv[2]),context.shader(argv[3]),VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,false,viewport);
        context.begin();VkClearValue clear{};VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=context.renderPass;begin.framebuffer=context.framebuffer;begin.renderArea={{0,0},{16,16}};begin.clearValueCount=1;begin.pClearValues=&clear;
        vkCmdBeginRenderPass(context.command,&begin,VK_SUBPASS_CONTENTS_INLINE);vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdDraw(context.command,3,2,0,0);vkCmdEndRenderPass(context.command);
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};image.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;image.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.image=context.image;image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers};
        vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers};copy.imageExtent={16,16,1};vkCmdCopyImageToBuffer(context.command,context.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,pixels.handle,1,&copy);
        context.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);context.finish();
        auto* bytes=static_cast<uint8_t*>(pixels.mapped);uint32_t correct=0,total=16*16*layers;
        for(uint32_t i=0;i<total;++i) {
            bool green=viewport?(i%16>=8):(i>=16*16);
            if(bytes[i*4]==(green?0:255)&&bytes[i*4+1]==(green?255:0)&&bytes[i*4+2]==0&&bytes[i*4+3]==255)++correct;
        }
        printf("{\"case\":\"%s\",\"correct\":%u,\"total\":%u,\"success\":%s}\n",viewport?"vertex-viewport":"vertex-layer",correct,total,correct==total?"true":"false");return correct==total?0:1;
    } catch(const std::exception& error) {fprintf(stderr,"layer probe failed: %s\n",error.what());return 1;}
}
