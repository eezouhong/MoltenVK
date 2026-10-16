#include "vulkan_context.h"
#include <cmath>
static uint32_t bits(float x){uint32_t v;memcpy(&v,&x,4);return v;}
int main(int argc,char**argv){try{
 if(argc!=2)return 64;
 constexpr float inputs[][3]={{1.00000011920928955078125f,1.00000011920928955078125f,-1.0000002384185791015625f},
 {-0.50000011920928955078125f,1.00000011920928955078125f,.5f},{3.f,.3333333432674407958984375f,-1.f},{.1f,10.f,-1.f}};
 Context context(false,false,false,true);auto storage=context.buffer(4*80,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
 auto* data=static_cast<uint32_t*>(storage.mapped);
 for(unsigned row=0;row<4;++row)for(unsigned lane=0;lane<4;++lane)for(unsigned j=0;j<3;++j)data[row*20+j*4+lane]=bits(inputs[(row+lane)%4][j]);
 context.output(storage,4*80);auto pipeline=context.compute(context.shader(argv[1]));context.begin();
 vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
 vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
 vkCmdDispatch(context.command,4,1,1);context.finish();unsigned correct=0,sensitive=0,loose=0;
 for(unsigned row=0;row<4;++row)for(unsigned lane=0;lane<4;++lane){auto& in=inputs[(row+lane)%4];
  volatile float product=in[0]*in[1];float expected=product+in[2];sensitive+=bits(expected)!=bits(std::fma(in[0],in[1],in[2]));
  uint32_t actual=data[row*20+12+lane];correct+=actual==bits(expected);
  volatile float ordinaryProduct=in[0]*in[2];float ordinary=ordinaryProduct+in[1];float fused=std::fma(in[0],in[2],in[1]);
  uint32_t other=data[row*20+16+lane];loose+=other==bits(ordinary)||other==bits(fused);
  if(actual!=bits(expected))fprintf(stderr,"row %u lane %u exact expected %08x got %08x\n",row,lane,bits(expected),actual);
 }
 bool ok=correct==16&&sensitive==16&&loose==16;
 printf("{\"correct\":%u,\"total\":16,\"fusionSensitive\":%u,\"looseAllowed\":%u,\"success\":%s}\n",correct,sensitive,loose,ok?"true":"false");return ok?0:1;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 2;}}
