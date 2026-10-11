#include "vulkan_context.h"
#include <dlfcn.h>

int main(int argc,char** argv) {
    if(argc!=7)return 64;
    alarm(45);
    try {
        const bool withFragment=std::string(argv[1])=="fragment";
        const bool indexed=std::string(argv[2])=="indexed";
        Context c(false,false,false,false,true);
        c.target(true);
        Buffer generated=c.buffer(16*sizeof(Row),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        c.output(generated,16*sizeof(Row));
        Buffer pixels=c.buffer(16*16*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkPipeline producer=c.graphics(c.shader(argv[3]),withFragment?c.shader(argv[5]):VK_NULL_HANDLE,
                                      VK_PRIMITIVE_TOPOLOGY_POINT_LIST,false,false,true);
        VkPipeline consumer=c.graphics(c.shader(argv[4]),c.shader(argv[6]),VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        Buffer indices=c.buffer(3*sizeof(uint32_t),VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        const uint32_t ids[]={7,8,9};memcpy(indices.mapped,ids,sizeof(ids));
        c.begin();
        VkClearValue clear{};VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass=c.renderPass;begin.framebuffer=c.framebuffer;begin.renderArea={{0,0},{16,16}};
        begin.clearValueCount=1;begin.pClearValues=&clear;
        vkCmdBeginRenderPass(c.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,producer);
        vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,c.layout,0,1,&c.descriptor,0,nullptr);
        uint32_t push[]={4,7,0,0,0,0,0,0};
        vkCmdPushConstants(c.command,c.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        if(indexed) { vkCmdBindIndexBuffer(c.command,indices.handle,0,VK_INDEX_TYPE_UINT32);vkCmdDrawIndexed(c.command,3,1,0,0,0); }
        else vkCmdDraw(c.command,3,1,7,0);
        vkCmdEndRenderPass(c.command);
        c.barrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_ACCESS_SHADER_READ_BIT);
        vkCmdBeginRenderPass(c.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,consumer);
        push[1]=0;
        vkCmdPushConstants(c.command,c.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        vkCmdDraw(c.command,3,1,0,0);
        vkCmdEndRenderPass(c.command);
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        image.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;image.image=c.image;
        image.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={16,16,1};
        vkCmdCopyImageToBuffer(c.command,c.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,pixels.handle,1,&copy);
        c.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_ACCESS_HOST_READ_BIT);
        c.finish();
        std::vector<Row> expected(16,Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});
        expected[4]={0xbf800000,0xbf800000,0,0x3f800000};
        expected[5]={0x40400000,0xbf800000,0,0x3f800000};
        expected[6]={0xbf800000,0x40400000,0,0x3f800000};
        bool stores=checkRows(generated,expected,"discard-producer-and-untouched-fragment-guard");
        uint32_t correct=0;auto* rgba=static_cast<uint8_t*>(pixels.mapped);
        for(uint32_t i=0;i<256;++i)if(rgba[i*4]==255&&rgba[i*4+1]==0&&rgba[i*4+2]==0&&rgba[i*4+3]==255)++correct;
        printf("{\"case\":\"discard-producer-to-visible-consumer\",\"pixels\":%u,\"total\":256,\"success\":%s}\n",correct,correct==256?"true":"false");
        Dl_info identity{};dladdr(reinterpret_cast<void*>(&vkCreateInstance),&identity);
        printf("{\"loadedNative\":\"%s\"}\n",identity.dli_fname?identity.dli_fname:"");
        return stores&&correct==256?0:1;
    } catch(const std::exception& e) { fprintf(stderr,"discard probe: %s\n",e.what());return 1; }
}
