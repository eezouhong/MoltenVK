#include "vulkan_context.h"
#include <cmath>
static uint32_t bits(float x){uint32_t v;memcpy(&v,&x,4);return v;}
int main(int argc,char**argv){try{
    if(argc<2||argc>3)return 64;
    bool half=argc==3;
    constexpr float fullInputs[][3]={{1.00000011920928955078125f,1.00000011920928955078125f,-1.0000002384185791015625f},
        {-0.50000011920928955078125f,1.00000011920928955078125f,0.5f},
        {3.f,0.3333333432674407958984375f,-1.f},{0.1f,10.f,-1.f}};
    constexpr float halfInputs[][3]={{64.0625f,1.0009765625f,-64.125f},{-64.0625f,1.0009765625f,64.125f},{64.0625f,-1.0009765625f,64.125f},{-64.0625f,-1.0009765625f,-64.125f}};
    const auto* inputs=half?halfInputs:fullInputs;
    constexpr unsigned rows=4;unsigned sensitive=0;
    for(unsigned i=0;i<rows;++i){auto& in=inputs[i];volatile float product=in[0]*in[1];float separated=half?float(_Float16(float(_Float16(product))+in[2])):product+in[2];float fused=std::fma(in[0],in[1],in[2]);if(half)fused=float(_Float16(fused));sensitive+=bits(separated)!=bits(fused);}
    if(sensitive!=rows)throw std::runtime_error("FMA sensitivity lost");
    Context context(false,false,false,true,false,false,false,false,true,half);
    fprintf(stderr,"STRONG_FMA_FEATURE_ENABLED\n");
    auto storage=context.buffer(rows*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto* data=static_cast<uint32_t*>(storage.mapped);
    for(unsigned i=0;i<rows;++i)for(unsigned j=0;j<3;++j)data[4*i+j]=bits(inputs[i][j]);
    context.output(storage,rows*16);auto pipeline=context.compute(context.shader(argv[1]));context.begin();
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
    vkCmdDispatch(context.command,rows,1,1);context.finish();unsigned correct=0;
    for(unsigned i=0;i<rows;++i){auto& in=inputs[i];float fused=std::fma(in[0],in[1],in[2]);if(half)fused=float(_Float16(fused));uint32_t expected=bits(fused);
        if(data[i*4+3]==expected)++correct;
        else fprintf(stderr,"row %u expected %08x got %08x\n",i,expected,data[i*4+3]);}
    printf("{\"correct\":%u,\"total\":%u,\"fusionSensitive\":%u,\"featureEnabled\":true,\"success\":%s}\n",
        correct,rows,sensitive,correct==rows?"true":"false");return correct==rows?0:1;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 2;}}
