#include "vulkan_context.h"
#include <climits>
#include <cstdint>

int main(int argc,char**argv) {try {
    if(argc!=2)return 64;
    // No zero divisor and no INT_MIN/-1: both are outside the defined oracle.
    constexpr int32_t inputs[][2]={{5,3},{-5,3},{5,-3},{-5,-3},
        {6,3},{-6,3},{6,-3},{-6,-3},{0,3},{0,-3},{1,-7},{-1,7},
        {INT_MAX,3},{INT_MIN,3},{INT_MAX,-3},{INT_MIN,-3},
        {INT_MIN,INT_MAX},{INT_MAX,INT_MIN},{INT_MIN,INT_MIN},{1,INT_MIN}};
    constexpr unsigned rows=sizeof(inputs)/sizeof(inputs[0]);
    Context context(false,false,false,true,false,false,false,true);
    fprintf(stderr,"SIGNED_MODULO_MAINTENANCE8_ENABLED\n");
    auto storage=context.buffer(rows*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto* data=static_cast<int32_t*>(storage.mapped);
    for(unsigned i=0;i<rows;++i){data[i*4]=inputs[i][0];data[i*4+1]=inputs[i][1];}
    context.output(storage,rows*16);
    auto pipeline=context.compute(context.shader(argv[1]));context.begin();
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
    vkCmdDispatch(context.command,rows,1,1);context.finish();
    unsigned modCorrect=0,remCorrect=0,inputCorrect=0,sensitive=0;
    for(unsigned i=0;i<rows;++i) {
        // Widen before arithmetic. The oracle is independent of GPU output.
        int64_t a=inputs[i][0],b=inputs[i][1],rem=a%b;
        int64_t mod=rem+((rem!=0&&(rem<0)!=(b<0))?b:0);
        int32_t unsignedResult=static_cast<int32_t>(uint32_t(a)%uint32_t(b));
        if(mod!=unsignedResult)++sensitive;
        modCorrect+=data[i*4+2]==mod;remCorrect+=data[i*4+3]==rem;
        inputCorrect+=data[i*4]==a&&data[i*4+1]==b;
        if(data[i*4+2]!=mod||data[i*4+3]!=rem)
            fprintf(stderr,"row %u a=%lld b=%lld expected mod=%lld rem=%lld got mod=%d rem=%d\n",
                i,(long long)a,(long long)b,(long long)mod,(long long)rem,data[i*4+2],data[i*4+3]);
    }
    bool success=modCorrect==rows&&remCorrect==rows&&inputCorrect==rows&&sensitive>0;
    printf("{\"modCorrect\":%u,\"remCorrect\":%u,\"inputCorrect\":%u,\"total\":%u,\"unsignedSensitive\":%u,\"success\":%s}\n",
           modCorrect,remCorrect,inputCorrect,rows,sensitive,success?"true":"false");
    return success?0:1;
}catch(const std::exception& error){fprintf(stderr,"%s\n",error.what());return 2;}}
