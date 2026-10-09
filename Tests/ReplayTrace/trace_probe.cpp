#include "vulkan_context.h"
#include "MVKReplayStubs.h"
#include <dlfcn.h>
int main(int argc,char** argv) {
    if(argc!=5)return 64;
    alarm(60);
    try {
        bool enabled=std::string(argv[1])=="on";
        Context context;context.target();
        Buffer output=context.buffer(64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);context.output(output,64);
        auto graphics=context.graphics(context.shader(argv[2]),context.shader(argv[3]),VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        auto compute=context.compute(context.shader(argv[4]));
        Buffer draw=context.buffer(sizeof(VkDrawIndirectCommand),VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        VkDrawIndirectCommand args{3,1,0,0};memcpy(draw.mapped,&args,sizeof(args));
        Buffer dispatch=context.buffer(12,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);uint32_t groups[3]={1,1,1};memcpy(dispatch.mapped,groups,12);
        context.begin();VkClearValue clear{};VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=context.renderPass;begin.framebuffer=context.framebuffer;begin.renderArea={{0,0},{16,16}};begin.clearValueCount=1;begin.pClearValues=&clear;
        vkCmdBeginRenderPass(context.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,graphics);
        vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,context.layout,0,1,&context.descriptor,0,nullptr);
        for(unsigned i=0;i<5000;++i)vkCmdDraw(context.command,3,1,0,0);
        vkCmdDrawIndirect(context.command,draw.handle,0,1,sizeof(args));vkCmdDrawIndirect(context.command,draw.handle,0,1,sizeof(args));
        vkCmdEndRenderPass(context.command);
        context.barrier(VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_SHADER_WRITE_BIT);
        vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,compute);
        vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
        vkCmdDispatchIndirect(context.command,dispatch.handle,0);context.finish();
        std::vector<Row> expected(4,Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});expected[0]={1,2,3,4};expected[1]={11,22,33,44};if(!checkRows(output,expected,"trace-gpu-output"))return 1;
        auto phase=reinterpret_cast<uint32_t(*)(mvkreplay::Sample*,uint32_t,VkBool32)>(dlsym(RTLD_DEFAULT,"vkGetReplayPhaseStatisticsMVK"));
        auto submission=reinterpret_cast<VkBool32(*)(mvkreplay::SubmissionSample*)>(dlsym(RTLD_DEFAULT,"vkGetReplaySubmissionStatisticsMVK"));
        auto binding=reinterpret_cast<uint32_t(*)(mvkreplay::BindingSample*,uint32_t)>(dlsym(RTLD_DEFAULT,"vkGetReplayBindingStatisticsMVK"));
        auto finish=reinterpret_cast<uint64_t(*)()>(dlsym(RTLD_DEFAULT,"vkFinishReplayFrameMVK"));
        if(!enabled) { if(phase||submission||binding||finish)return 2;puts("{\"traceEnabled\":false,\"exportsAbsent\":true,\"success\":true}");return 0; }
        if(!phase||!submission||!binding||!finish)return 3;
        mvkreplay::Sample phases[mvkreplay::RegionCount]{};if(phase(phases,mvkreplay::RegionCount,VK_FALSE)!=mvkreplay::RegionCount)return 4;
        if(!phases[mvkreplay::MetalCommandEncoding].calls||!phases[mvkreplay::MetalGraphicsPSO].calls||!phases[mvkreplay::MSLLibrary].calls)return 5;
        mvkreplay::BindingSample bindings[mvkreplay::bindingCounterCount]{};if(binding(bindings,mvkreplay::bindingCounterCount)!=mvkreplay::bindingCounterCount)return 6;
        unsigned metal=unsigned(mvkreplay::BindingGroup::MetalDraw)*2;
        if(!bindings[metal].calls||!bindings[metal].samples||!bindings[metal].cpuNs)return 7;
        mvkreplay::SubmissionSample totals{};if(!submission(&totals))return 8;
        if(!totals.buffers||totals.errors)return 9;
        if(finish()!=1)return 10;
        printf("{\"traceEnabled\":true,\"encodedBatches\":%llu,\"drawSamples\":%llu,\"drawCpuNs\":%llu,\"gpuBuffers\":%llu,\"success\":true}\n",(unsigned long long)phases[mvkreplay::MetalCommandEncoding].calls,(unsigned long long)bindings[metal].samples,(unsigned long long)bindings[metal].cpuNs,(unsigned long long)totals.buffers);
        return 0;
    }catch(const std::exception& e){fprintf(stderr,"trace probe failed: %s\n",e.what());return 1;}
}
