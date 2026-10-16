#include "vulkan_context.h"
#include <cmath>
// Exact answers for strong_fma_rules{,16}.spvasm. Each row also records what a
// compiler that splits the fusion or folds signed zero would store; a row whose
// two answers coincide cannot detect that rewrite, so the probe refuses to run.
static uint32_t bits(float x){uint32_t v;memcpy(&v,&x,4);return v;}
struct Case{const char* name;float expected,rewritten;};
int main(int argc,char**argv){try{
    if(argc<2||argc>3)return 64;
    bool half=argc==3;
    constexpr unsigned inputs=20,base=32,rows=12;
    // f32 rows fuse to 2^-46 and split to 0; f16 rows fuse to 2^-14 and split to 0.
    const float a=half?64.0625f:1.00000011920928955078125f,b=half?1.0009765625f:1.00000011920928955078125f,
                c=half?-64.125f:-1.0000002384185791015625f,scale=half?16384.f:70368744177664.f;
    constexpr float fullLanes[][3]={{1.00000011920928955078125f,1.00000011920928955078125f,-1.0000002384185791015625f},
        {-0.50000011920928955078125f,1.00000011920928955078125f,0.5f},
        {3.f,0.3333333432674407958984375f,-1.f},{0.1f,10.f,-1.f}};
    constexpr float halfLanes[][3]={{64.0625f,1.0009765625f,-64.125f},{-64.0625f,1.0009765625f,64.125f},{64.0625f,-1.0009765625f,64.125f},{-64.0625f,-1.0009765625f,-64.125f}};
    const auto* lanes=half?halfLanes:fullLanes;
    float x[inputs]={a,b,c,-0.f,1.f,scale,-1.f,0.f};
    for(unsigned i=0;i<4;++i)for(unsigned j=0;j<3;++j)x[8+4*j+i]=lanes[i][j];
    // Half products of half inputs are exact in double, so one final narrowing is the single rounding.
    auto fused=[&](float p,float q,float r){return half?float(_Float16(std::fma(double(p),double(q),double(r)))):std::fma(p,q,r);};
    auto split=[&](float p,float q,float r){if(half)return float(_Float16(double(_Float16(double(p)*q))+r));volatile float m=p*q;return m+r;};
    Case expect[rows]={{"dynamic",fused(a,b,c),split(a,b,c)},
        {"zero-constant-addend",fused(-0.f,1.f,0.f),-0.f},
        {"zero-constant-one-multiplier",fused(-0.f,1.f,0.f),-0.f},
        {"zero-dynamic-addend",fused(-0.f,1.f,0.f),-0.f},
        {"constant-operands",fused(a,b,c),split(a,b,c)},
        {"all-constant",fused(a,b,c),split(a,b,c)},
        {"negated-sources",fused(-a,b,-c),split(-a,b,-c)},
        {"chained",fused(fused(a,b,c),scale,-1.f),split(split(a,b,c),scale,-1.f)}};
    static const char* laneNames[]={"vector-lane-0","vector-lane-1","vector-lane-2","vector-lane-3"};
    for(unsigned i=0;i<4;++i)expect[8+i]={laneNames[i],fused(lanes[i][0],lanes[i][1],lanes[i][2]),split(lanes[i][0],lanes[i][1],lanes[i][2])};
    for(auto& row:expect)if(bits(row.expected)==bits(row.rewritten))throw std::runtime_error(std::string("row cannot detect its rewrite: ")+row.name);
    Context context(false,false,false,true,false,false,false,false,true,half);
    fprintf(stderr,"STRONG_FMA_FEATURE_ENABLED\n");
    auto storage=context.buffer((base+rows)*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto* data=static_cast<uint32_t*>(storage.mapped);
    for(unsigned i=0;i<base+rows;++i)data[i]=i<inputs?bits(x[i]):0x7fc0deadu;
    context.output(storage,(base+rows)*4);auto pipeline=context.compute(context.shader(argv[1]));context.begin();
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
    vkCmdDispatch(context.command,1,1,1);context.finish();unsigned correct=0;std::string report;
    for(unsigned i=0;i<rows;++i){auto& row=expect[i];uint32_t got=data[base+i],want=bits(row.expected);bool pass=got==want;correct+=pass;
        if(!pass)fprintf(stderr,"row %s expected %08x got %08x rewrite %08x\n",row.name,want,got,bits(row.rewritten));
        char item[192];snprintf(item,sizeof item,"%s{\"name\":\"%s\",\"expected\":\"%08x\",\"got\":\"%08x\",\"rewritten\":\"%08x\",\"pass\":%s}",
            i?",":"",row.name,want,got,bits(row.rewritten),pass?"true":"false");report+=item;}
    printf("{\"correct\":%u,\"total\":%u,\"rows\":[%s],\"featureEnabled\":true,\"success\":%s}\n",
        correct,rows,report.c_str(),correct==rows?"true":"false");return correct==rows?0:1;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 2;}}
