#include "vulkan_context.h"
// Pixel values are checked against CPU constants/coordinates, not another compiler.
int main(int argc, char** argv) {
    if (argc != 4) return 64;
    alarm(30);
    try {
        Context c(false, false, false, false, true); c.target(true);
        Buffer output=c.buffer(256*sizeof(Row), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        c.output(output,256*sizeof(Row));
        auto pipeline=c.graphics(c.shader(argv[1]),c.shader(argv[2]),VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        c.begin(); VkClearValue clear{};
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass=c.renderPass; pass.framebuffer=c.framebuffer;
        pass.renderArea={{0,0},{16,16}}; pass.clearValueCount=1; pass.pClearValues=&clear;
        vkCmdBeginRenderPass(c.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,c.layout,0,1,&c.descriptor,0,nullptr);
        vkCmdDraw(c.command,3,1,0,0); vkCmdEndRenderPass(c.command); c.finish();
        std::vector<Row> expected;
        for(unsigned y=0;y<16;++y) for(unsigned x=0;x<16;++x)
            expected.push_back(std::string(argv[3])=="gradient" ? Row{2*x+1,2*y+1,16,32} : Row{64,128,191,255});
        return checkRows(output,expected,argv[3]) ? 0 : 1;
    } catch(const std::exception& e) { fprintf(stderr,"varying probe: %s\n",e.what()); return 1; }
}
