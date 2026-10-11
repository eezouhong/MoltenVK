#include "vulkan_context.h"
#include <unistd.h>
int main(int argc,char** argv) {
    if(argc!=3)return 64;
    alarm(70);
    try {
        Context context;
        context.target();
        constexpr unsigned draws=60, rows=draws*16;
        Buffer output=context.buffer(rows*sizeof(Row),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        context.output(output,rows*sizeof(Row));
        Buffer indices=context.buffer(12,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        uint32_t indexData[3]={7,8,9};memcpy(indices.mapped,indexData,sizeof(indexData));
        auto pipeline=context.graphics(context.shader(argv[1]),context.shader(argv[2]),VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        std::vector<Row> expected(rows,Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});
        context.begin();
        VkClearValue clear{};VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass=context.renderPass;begin.framebuffer=context.framebuffer;
        begin.renderArea={{0,0},{16,16}};begin.clearValueCount=1;begin.pClearValues=&clear;
        vkCmdBeginRenderPass(context.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,context.layout,0,1,&context.descriptor,0,nullptr);
        vkCmdBindIndexBuffer(context.command,indices.handle,0,VK_INDEX_TYPE_UINT32);
        for(unsigned d=0;d<draws;++d) {
            bool indexed=d%2;int32_t offset=indexed ? -11+int32_t(d) : 0;
            int32_t first=indexed ? 7+offset : 7+int32_t(d)*3;
            uint32_t instance=5+d*2,marker=0x600d0000+d;
            uint32_t push[8]={d*16,uint32_t(first),instance,marker,0,0,0,0};
            vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
            for(unsigned i=0;i<2;++i)for(unsigned v=0;v<3;++v) {
                unsigned slot=d*16+i*6+v*2;
                expected[slot]={uint32_t(first+int32_t(v)),instance+i,marker,0xabcdef01};
                expected[slot+1]={v,i,d*16,0x12345678};
            }
            if(indexed)vkCmdDrawIndexed(context.command,3,2,0,offset,instance);
            else vkCmdDraw(context.command,3,2,uint32_t(first),instance);
        }
        vkCmdEndRenderPass(context.command);context.finish();
        return checkRows(output,expected,"native-absolute-ids")?0:1;
    } catch(const std::exception& error) {fprintf(stderr,"native IDs: %s\n",error.what());return 1;}
}
