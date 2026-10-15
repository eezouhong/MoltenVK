#include "vulkan_context.h"
#include <cmath>
static uint32_t bits(float value) { uint32_t result; memcpy(&result,&value,4);return result; }
static float value(uint32_t word) { float result; memcpy(&result,&word,4);return result; }
int main(int argc,char**argv) {try {
    if(argc!=2)return 64;
    Context context(false,false,false,true);
    // Rows 0-255 are every material byte with the guest's 0.5 depth bias; the
    // rest are scalar fma cases whose fused and separated roundings differ.
    const float extra[][3]={{-0.500000119209289551f,1.00000012f,.5f},{1.00000012f,1.00000012f,-1.00000024f},
                            {3.f,0.333333343f,-1.f},{0.1f,10.f,-1.f}};
    constexpr unsigned rows=256+sizeof(extra)/sizeof(extra[0]);
    for(auto& row:extra) {
        volatile float product=row[0]*row[1];
        if(bits(std::fma(row[0],row[1],row[2]))==bits(product+row[2]))
            throw std::runtime_error("fma sentinel does not distinguish fusion");
    }
    auto storage=context.buffer(rows*5*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto* data=static_cast<uint32_t*>(storage.mapped);
    for(unsigned i=0;i<rows;++i) {
        const float in[3]={i<256?float(i):extra[i-256][0],i<256?1.f:extra[i-256][1],i<256?.5f:extra[i-256][2]};
        for(unsigned j=0;j<3;++j)data[i*5+j]=bits(in[j]);
    }
    context.output(storage,rows*5*4);
    auto pipeline=context.compute(context.shader(argv[1]));context.begin();
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
    vkCmdDispatch(context.command,rows,1,1);context.finish();
    unsigned correct=0,sensitive=0;
    for(unsigned i=0;i<rows;++i) {
        float a=value(data[i*5]),b=value(data[i*5+1]),c=value(data[i*5+2]);
        volatile float scaled=a*0.0039215688593685626983642578125f;
        float fused=std::fma(a,b,c),chain=std::fma(scaled,-c,scaled)+c;
        // The regrouping a Relaxed split multiply/add allowed before fusion was kept.
        volatile float bias=c*scaled;float regrouped=std::fma(a,0.0039215688593685626983642578125f,c-bias);
        if(bits(chain)!=bits(regrouped))++sensitive;
        if(bits(fused)==data[i*5+3]&&bits(chain)==data[i*5+4])++correct;
        else fprintf(stderr,"row %u expected %08x/%08x got %08x/%08x\n",i,bits(fused),bits(chain),data[i*5+3],data[i*5+4]);
    }
    if(!sensitive)throw std::runtime_error("depth chain does not distinguish regrouping");
    printf("{\"correct\":%u,\"total\":%u,\"regroupSensitive\":%u,\"success\":%s}\n",correct,rows,sensitive,correct==rows?"true":"false");
    return correct==rows?0:1;
}catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 1;}}
