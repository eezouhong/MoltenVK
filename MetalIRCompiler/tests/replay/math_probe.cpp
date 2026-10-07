#include "vulkan_context.h"
#include <cmath>
#include <limits>
static uint32_t bits(float value) { uint32_t result; memcpy(&result,&value,4);return result; }
int main(int argc,char**argv) {try {
    if(argc<2||argc>4)return 64;
    bool requirePrecise=argc<3||strcmp(argv[2],"relaxed");
    bool finiteOnly=argc==4&&!strcmp(argv[3],"finite");
    Context context(false,false,false,true);
    const float cases[]={0.f,-0.f,1.f,-1.f,2.f,-2.f,.25f,INFINITY,-INFINITY,NAN,
                         -0.99999988f,1000.f,-1000.f,0.000001f,-0.500000119209289551f,-0.500000178813934326f};
    const float finiteCases[]={0.f,-0.f,1.f,-.5f,2.f,-2.f,.25f,.5f,-.25f,1.5f,
                               -0.99999988f,1000.f,-1000.f,0.000001f,-0.500000119209289551f,-0.500000178813934326f};
    volatile float sentinelProduct=cases[14]*1.00000012f;
    float sentinelSeparated=sentinelProduct+0.5f;
    if(bits(std::fma(cases[14],1.00000012f,0.5f))==bits(sentinelSeparated))
        throw std::runtime_error("NoContraction sentinel does not distinguish FMA");
    auto* inputs=finiteOnly?finiteCases:cases;
    auto storage=context.buffer(64*3*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto* data=static_cast<uint32_t*>(storage.mapped);
    for(unsigned i=0;i<64;++i)data[i*3]=bits(inputs[i%16]);
    context.output(storage,64*3*4);
    auto pipeline=context.compute(context.shader(argv[1]));context.begin();
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
    vkCmdDispatch(context.command,64,1,1);context.finish();
    unsigned correct=0,precise=0;
    for(unsigned i=0;i<64;++i) {
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
    printf("{\"correct\":%u,\"precise\":%u,\"total\":64,\"success\":%s}\n",correct,precise,correct==64?"true":"false");
    return correct==64?0:1;
}catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 1;}}
