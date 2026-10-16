#include "vulkan_context.h"
#include <cmath>
#include <cstdint>
using Allowed=std::vector<uint32_t>;
static float val(uint32_t x){float v;memcpy(&v,&x,4);return v;}
static bool nan(uint32_t x){return std::isnan(val(x));}
static bool zero(uint32_t x){return (x&0x7fffffff)==0;}
static Allowed pair(uint32_t a,uint32_t b,bool number,bool maximum,bool looseZero){
 if(nan(a)||nan(b)){if(!number)return {a,b};if(nan(a))return {b};return {a};}
 if(zero(a)&&zero(b)){if(looseZero)return {0,0x80000000u};return {maximum?(a&b):(a|b)};}
 return {maximum?(val(a)<val(b)?b:a):(val(b)<val(a)?b:a)};
}
static Allowed clamp(uint32_t x,uint32_t lo,uint32_t hi,bool number,bool looseZero){Allowed out;
 for(auto m:pair(x,lo,number,true,looseZero)){auto last=pair(m,hi,number,false,looseZero);out.insert(out.end(),last.begin(),last.end());}return out;}
static bool accepts(const Allowed& set,uint32_t got,bool looseZero){for(auto v:set)if(v==got||(nan(v)&&nan(got))||(looseZero&&zero(v)&&zero(got)))return true;return false;}
int main(int argc,char**argv){try{
 if(argc!=3)return 64;std::string mode=argv[2];bool fast=mode=="fast",loose=mode!="safe";
 constexpr uint32_t P=0,I=0x7f800000,N=0xff800000,Q=0x7fc01234,Z=0x80000000;
 const std::array<uint32_t,4> inputs[]={
 {0xbf800000,0x40000000,P,0x3f800000},{0x3f000000,0xbf000000,0xbf800000,0x3f800000},
 {0x40000000,0x3f800000,0xc0000000,0x40000000},{P,Z,Z,P},{Z,P,Z,P},{Z,Z,Z,Z},
 {Q,0x3f800000,0xbf800000,0x3f800000},{0x3f800000,Q,0xbf800000,0x3f800000},{Q,Q,P,0x3f800000},
 {I,N,N,I},{N,I,N,I},{I,0x3f800000,P,I},{N,0xbf800000,N,P},
 {Q,P,Q,0x3f800000},{Q,P,0xbf800000,Q},{0x3f000000,Q,Q,Q}};
 unsigned rows=fast?6:sizeof(inputs)/sizeof(inputs[0]);Context context(false,false,false,true);
 auto storage=context.buffer(rows*64,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);auto* data=(uint32_t*)storage.mapped;
 for(unsigned i=0;i<rows;++i)for(unsigned j=0;j<4;++j)data[i*16+j]=inputs[i][j];
 context.output(storage,rows*64);auto pipeline=context.compute(context.shader(argv[1]));context.begin();
 vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
 vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
 vkCmdDispatch(context.command,rows,1,1);context.finish();unsigned correct[12]={};
 for(unsigned i=0;i<rows;++i){auto in=inputs[i];uint32_t x=in[0],y=in[1],lo=in[2],hi=in[3];
  Allowed expected[]={pair(x,y,false,false,loose),pair(y,x,false,false,loose),pair(x,y,false,true,loose),pair(y,x,false,true,loose),
   pair(x,y,true,false,loose),pair(y,x,true,false,loose),pair(x,y,true,true,loose),pair(y,x,true,true,loose),
   clamp(x,lo,hi,false,loose),clamp(x,lo,hi,true,loose),clamp(x,P,0x3f800000,false,loose),clamp(x,P,0x3f800000,true,loose)};
  for(unsigned op=0;op<12;++op){auto got=data[i*16+4+op];if(accepts(expected[op],got,loose))++correct[op];
   else{fprintf(stderr,"row %u op %u got=%08x allowed=",i,op,got);for(auto e:expected[op])fprintf(stderr,"%08x,",e);fprintf(stderr,"\n");}}
 }
 bool ok=true;printf("{\"perOpCorrect\":[");for(unsigned op=0;op<12;++op){printf("%s%u",op?",":"",correct[op]);ok&=correct[op]==rows;}
 printf("],\"rows\":%u,\"success\":%s}\n",rows,ok?"true":"false");return ok?0:1;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 2;}}
