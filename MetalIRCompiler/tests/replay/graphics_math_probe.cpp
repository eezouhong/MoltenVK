#include "vulkan_context.h"
#include <cmath>
#include <limits>
static uint32_t bits(float value) { uint32_t result; memcpy(&result,&value,4);return result; }
int main(int argc,char**argv) {try {
    if(argc!=4)return 64;
    bool vertex=!strcmp(argv[1],"vertex"); bool requirePrecise=true;
    bool finiteOnly=false; unsigned count=vertex?64:256;
    Context context(false,false,false,true,true);
    const float cases[]={0.f,-0.f,1.f,-1.f,2.f,-2.f,.25f,INFINITY,-INFINITY,NAN,
                         -0.99999988f,1000.f,-1000.f,0.000001f,-0.500000119209289551f,-0.500000178813934326f};
    const float finiteCases[]={0.f,-0.f,1.f,-.5f,2.f,-2.f,.25f,.5f,-.25f,1.5f,
                               -0.99999988f,1000.f,-1000.f,0.000001f,-0.500000119209289551f,-0.500000178813934326f};
    volatile float sentinelProduct=cases[14]*1.00000012f;
    float sentinelSeparated=sentinelProduct+0.5f;
    if(bits(std::fma(cases[14],1.00000012f,0.5f))==bits(sentinelSeparated))
        throw std::runtime_error("NoContraction sentinel does not distinguish FMA");
    auto* inputs=finiteOnly?finiteCases:cases;
    auto storage=context.buffer(count*3*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto* data=static_cast<uint32_t*>(storage.mapped);
    for(unsigned i=0;i<count;++i)data[i*3]=bits(inputs[i%16]);
    context.output(storage,count*3*4);
    context.target();
    auto pipeline=context.graphics(context.shader(argv[2]),context.shader(argv[3]),VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    context.begin();VkClearValue clear{};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};pass.renderPass=context.renderPass;
    pass.framebuffer=context.framebuffer;pass.renderArea={{0,0},{16,16}};pass.clearValueCount=1;pass.pClearValues=&clear;
    vkCmdBeginRenderPass(context.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,context.layout,0,1,&context.descriptor,0,nullptr);
    vkCmdDraw(context.command,3,vertex?64:1,0,0);
    vkCmdEndRenderPass(context.command);context.finish();
    unsigned correct=0,precise=0;
    for(unsigned i=0;i<count;++i) {
        float a=inputs[i%16]; volatile float product=a*1.00000012f;volatile float exact=product+0.5f;
        volatile float square=a*a;volatile float numerator=square+.25f;volatile float denominator=a+1.f;
        volatile float ordinary=numerator/denominator;float expected=ordinary+exact;
        float gotExact,got;memcpy(&gotExact,&data[i*3+1],4);memcpy(&got,&data[i*3+2],4);
        bool exactBits=std::isnan(exact)?std::isnan(gotExact):bits(exact)==data[i*3+1];
        bool exactOK=requirePrecise?exactBits:std::isnan(exact)?std::isnan(gotExact):std::isinf(exact)?bits(exact)==bits(gotExact):
            std::isfinite(gotExact)&&std::abs(gotExact-exact)<=std::max(1e-6f,std::abs(exact)*2e-6f);
        bool valueOK=std::isnan(expected)?std::isnan(got):std::isinf(expected)?bits(expected)==bits(got):
            std::isfinite(got)&&std::abs(got-expected)<=std::max(1e-6f,std::abs(expected)*2e-6f);
        if(exactBits)++precise;
        if(exactOK&&valueOK&&data[i*3]==bits(a))++correct;
        else fprintf(stderr,"row %u expected precise=%08x result=%08x got=%08x/%08x\n",i,bits(exact),bits(expected),data[i*3+1],data[i*3+2]);
    }
    printf("{\"correct\":%u,\"precise\":%u,\"total\":%u,\"success\":%s}\n",correct,precise,count,correct==count?"true":"false");
    return correct==count?0:1;
}catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 1;}}
