#include "vulkan_context.h"
// Rows of nan_default.spvasm. "preserved" is the result when NaN and Inf are
// honored; the legacy module declares no float controls, so Vulkan permits
// other results. The runner requires "preserved" only where the math mode does.
struct Expected{const char* name;uint32_t preserved;};
int main(int argc,char**argv){try{
    if(argc!=2)return 64;
    const Expected rows[]={{"one-minus-clamp-nan",0x3f800000u},{"isnan-inf-minus-inf",1},{"isinf-inf-times-two",1},
        {"isnan-inf-times-zero",1},{"nan-equals-self",0}};
    constexpr unsigned base=8,count=sizeof(rows)/sizeof(rows[0]);
    Context context;auto storage=context.buffer(16*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    auto* data=static_cast<uint32_t*>(storage.mapped);
    const uint32_t inputs[]={0x7fc00000u,0x7f800000u,0x40000000u,0};
    for(unsigned i=0;i<16;++i)data[i]=i<4?inputs[i]:0xdeadbeefu;
    context.output(storage,16*4);auto pipeline=context.compute(context.shader(argv[1]));context.begin();
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
    vkCmdDispatch(context.command,1,1,1);context.finish();unsigned preserved=0;std::string report;
    for(unsigned i=0;i<count;++i){uint32_t got=data[base+i];bool same=got==rows[i].preserved;preserved+=same;
        char item[160];snprintf(item,sizeof item,"%s{\"name\":\"%s\",\"got\":\"%08x\",\"preserved\":\"%08x\",\"matches\":%s}",
            i?",":"",rows[i].name,got,rows[i].preserved,same?"true":"false");report+=item;}
    printf("{\"preserved\":%u,\"total\":%u,\"rows\":[%s],\"success\":true}\n",preserved,count,report.c_str());return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 2;}}
