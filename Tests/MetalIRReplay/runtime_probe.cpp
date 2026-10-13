#include "vulkan_context.h"
#include <dlfcn.h>

static const char* cacheDirectory=nullptr;
static void checkResidency(Context& context) {
    if (!getenv("MELONX_RESIDENCY_PROBE")) return;
    auto query=reinterpret_cast<uint32_t(*)(VkDevice,uint64_t*,uint32_t)>(dlsym(RTLD_DEFAULT,"vkGetShaderLibraryResidencyStatisticsMVK"));
    auto trim=reinterpret_cast<VkResult(*)(VkDevice)>(dlsym(RTLD_DEFAULT,"vkTrimShaderLibraryResidencyMVK"));
    uint64_t before[16]{}, repeated[16]{}, after[16]{};
    const bool ir=getenv("MELONX_EXPERIMENTAL_METAL_IR") && !strcmp(getenv("MELONX_EXPERIMENTAL_METAL_IR"),"1");
    if (!query || !trim || query(VK_NULL_HANDLE,before,16)!=0 || query(context.device,before,15)!=0 ||
        trim(VK_NULL_HANDLE)!=VK_ERROR_INITIALIZATION_FAILED || query(context.device,before,16)!=16 ||
        query(context.device,repeated,16)!=16 || memcmp(before,repeated,sizeof(before)))
        throw std::runtime_error("residency query invalid or mutated state");
    if (before[0]!=1 || !before[1] || before[2]!=(ir?1:2) || before[4]!=(ir?64:512) ||
        before[13]!=(ir?1:2) || before[14]!=(ir?1:2) || !before[15])
        throw std::runtime_error("residency scope, payload or default changed");
    VK_CHECK(trim(context.device));
    if (query(context.device,after,16)!=16 || before[3]!=after[3] || before[4]!=after[4] ||
        before[5]!=after[5] || before[6]!=after[6] || before[7]!=after[7] || before[8]!=after[8])
        throw std::runtime_error("under-budget trim changed retained work");
    fprintf(stderr,"MELONX_RESIDENCY {\"route\":\"%s\",\"entries\":%llu,\"limit\":%llu,\"evictions\":%llu,\"restores\":%llu,\"readOnly\":true,\"trimPreserved\":true}\n",
        ir?"IR":"MSL",(unsigned long long)before[3],(unsigned long long)before[4],
        (unsigned long long)before[5],(unsigned long long)before[6]);
}
static void configureCache(Context& context) {
    if(!cacheDirectory)return;
    if (getenv("MELONX_BASIC_COUNTER_PROBE")) {
        auto statistics = reinterpret_cast<uint32_t(*)(VkDevice,uint64_t*,uint32_t)>(dlsym(RTLD_DEFAULT,"vkGetMetalIRCompilerStatisticsMVK"));
        auto enable = reinterpret_cast<uint32_t(*)(uint32_t)>(dlsym(RTLD_DEFAULT,"vkSetMetalIRTelemetryEnabledMVK"));
        uint64_t before[9]{};
        if (!statistics || !enable || statistics(context.device,before,9)!=0 || enable(1)!=0)
            throw std::runtime_error("late basic-counter activation did not preserve disabled first-read state");
    }
    auto configure=reinterpret_cast<VkResult(*)(VkDevice,const char*,uint64_t)>(dlsym(RTLD_DEFAULT,"vkConfigureMetalIRCacheMVK"));
    if(!configure)throw std::runtime_error("missing per-device IR cache configuration");
    VK_CHECK(configure(context.device,cacheDirectory,2ull*1024*1024*1024));
}
static void printCacheStatistics(Context& context) {
    checkResidency(context);
    if(!cacheDirectory)return;
    auto statistics=reinterpret_cast<uint32_t(*)(VkDevice,uint64_t*,uint32_t)>(dlsym(RTLD_DEFAULT,"vkGetMetalIRCompilerStatisticsMVK"));
    uint64_t values[9]{},legacy[7]{};
    if(!statistics||statistics(context.device,values,9)!=9||statistics(context.device,legacy,7)!=7)
        throw std::runtime_error("missing or incompatible IR cache statistics");
    if(!values[8]||values[7])throw std::runtime_error("missing PSO timing or unexpected shader rejection");
    for(unsigned i=0;i<7;++i)if(values[i]!=legacy[i])throw std::runtime_error("legacy statistics changed");
    fprintf(stderr,"MELONX_REPLAY_CACHE {\"compiled\":%llu,\"restored\":%llu,\"mesaNs\":%llu,\"mscNs\":%llu,\"rejected\":%llu,\"psoNs\":%llu}\n",
        (unsigned long long)values[0],(unsigned long long)values[1],(unsigned long long)values[2],(unsigned long long)values[3],
        (unsigned long long)values[7],(unsigned long long)values[8]);
    if (getenv("MELONX_BASIC_COUNTER_PROBE")) {
        auto memory = reinterpret_cast<uint32_t(*)(VkDevice,uint64_t*,uint32_t)>(dlsym(RTLD_DEFAULT,"vkGetMetalIRCacheStatisticsMVK"));
        uint64_t fields[40]{}, legacyMemory[37]{};
        if (!memory || memory(context.device,fields,40)!=40 || memory(context.device,legacyMemory,37)!=37 ||
            fields[0]!=1 || (fields[1]&7)!=7 ||
            fields[2]!=512 || fields[3]!=64*1024*1024 || !fields[5] || !fields[11] || !fields[13])
            throw std::runtime_error("basic snapshot unavailable, incomplete or budget changed");
        // Disk-write and device-wide counters can change between read-only calls.
        for (unsigned i=0; i<18; ++i)
            if (fields[i]!=legacyMemory[i])
                throw std::runtime_error("legacy memory statistics prefix changed");
        // Attempts include rejected Metal libraries; successful restores are a subset.
        if (fields[37]<fields[34] || (fields[37] && (!fields[38] || !fields[39] || fields[39]>fields[38])) ||
            (!fields[37] && (fields[38] || fields[39])))
            throw std::runtime_error("disk library creation timing missing or inconsistent");
        if (dlsym(RTLD_DEFAULT,"vkGetReplayPhaseStatisticsMVK"))
            throw std::runtime_error("basic counter probe requires a trace-disabled native build");
        fprintf(stderr,"MELONX_REPLAY_MEMORY {\"flags\":%llu,\"retained\":%llu,\"liveLibraries\":%llu,\"created\":%llu,\"successfulRestores\":%llu,\"diskLibraryCreateCount\":%llu,\"diskLibraryCreateNs\":%llu,\"diskLibraryCreateMaxNs\":%llu}\n",
            (unsigned long long)fields[1],(unsigned long long)fields[5],(unsigned long long)fields[11],
            (unsigned long long)fields[13],(unsigned long long)fields[34],(unsigned long long)fields[37],
            (unsigned long long)fields[38],(unsigned long long)fields[39]);
    }
}

// Seal the existing headless replay epoch after the fixture's only submission.
// Disabled in the ordinary correctness suite; no swapchain/present is added.
static void finishReplayFrame() {
    const char* trace=getenv("MELONX_PIPELINE_REPLAY_TRACE");
    if (!trace || strcmp(trace,"coarse")) return;
    auto finish=reinterpret_cast<uint64_t(*)()>(dlsym(RTLD_DEFAULT,"vkFinishReplayFrameMVK"));
    if (!finish || finish()!=1) throw std::runtime_error("missing or invalid replay frame epoch");
}

static bool graphics(const char* vertex,const char* fragment,bool fans, bool tail=false, bool fanIndirect=false) {
    Context context; configureCache(context); context.target();
    const uint32_t total=512;
    Buffer output=context.buffer(total*sizeof(Row),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); context.output(output,total*sizeof(Row));
    Buffer indices=context.buffer(256,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    Buffer indices16=context.buffer(128,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    auto* i32=reinterpret_cast<uint32_t*>(static_cast<char*>(indices.mapped)+16);
    auto* i16=reinterpret_cast<uint16_t*>(static_cast<char*>(indices16.mapped)+8);
    for(uint32_t i=0;i<3;++i) { i32[2+i]=7+i; i32[8+i]=20+i; i16[2+i]=20+i; }
    const uint32_t nonOffset=tail?4:32, indexedOffset=tail?8:128;
    Buffer indirect=context.buffer(tail?indexedOffset+32+20:256,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Buffer nonIndexed=context.buffer(tail?nonOffset+32+16:128,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    auto pipeline=context.graphics(context.shader(vertex),context.shader(fragment), fans?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    std::vector<Row> expected(total,Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});
    context.begin();
    if(!fans || fanIndirect) {
        VkDrawIndirectCommand first{3,2,10,3},second{3,2,20,7};
        uint32_t commands[16]{};
        memcpy(commands,&first,sizeof(first)); memcpy(commands+8,&second,sizeof(second));
        vkCmdUpdateBuffer(context.command,nonIndexed.handle,nonOffset,32+sizeof(second),commands);
        VkDrawIndexedIndirectCommand indexedFirst{3,2,2,-4,5},indexedSecond{3,2,8,6,7};
        memcpy(commands,&indexedFirst,sizeof(indexedFirst)); memcpy(commands+8,&indexedSecond,sizeof(indexedSecond));
        vkCmdUpdateBuffer(context.command,indirect.handle,indexedOffset,32+sizeof(indexedSecond),commands);
        context.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    }
    VkClearValue clear{}; VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; begin.renderPass=context.renderPass; begin.framebuffer=context.framebuffer;
    begin.renderArea={{0,0},{16,16}}; begin.clearValueCount=1; begin.pClearValues=&clear;
    vkCmdBeginRenderPass(context.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,context.layout,0,1,&context.descriptor,0,nullptr);
    auto prepare=[&](uint32_t destination, std::array<int32_t,2> origins, std::array<uint32_t,2> instances,std::array<int32_t,2> bases,uint32_t draws=1) {
        uint32_t push[8]={destination,0,static_cast<uint32_t>(origins[0]),static_cast<uint32_t>(origins[1]),instances[0],instances[1],0,0};
        vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        for(uint32_t d=0;d<draws;++d) for(uint32_t i=0;i<2;++i) for(uint32_t v=0;v<3;++v) {
            auto slot=destination+d*12+i*6+v*2;
            expected[slot]={static_cast<uint32_t>(origins[d])+v,instances[d]+i,static_cast<uint32_t>(bases[d]),instances[d]};
            expected[slot+1]={d,0xabcdef01,static_cast<uint32_t>(origins[d]),instances[d]};
        }
    };
    prepare(0,{7,0},{5,0},{7,0}); vkCmdDraw(context.command,3,2,7,5);
    prepare(32,{11,0},{2,0},{11,0}); vkCmdDraw(context.command,3,2,11,2);
    vkCmdBindIndexBuffer(context.command,indices.handle,16,VK_INDEX_TYPE_UINT32);
    prepare(64,{3,0},{9,0},{-4,0}); vkCmdDrawIndexed(context.command,3,2,2,-4,9);
    vkCmdBindIndexBuffer(context.command,indices16.handle,8,VK_INDEX_TYPE_UINT16);
    prepare(96,{12,0},{1,0},{-8,0}); vkCmdDrawIndexed(context.command,3,2,2,-8,1);
    if(!fans || fanIndirect) {
        prepare(128,{10,20},{3,7},{10,20},2); vkCmdDrawIndirect(context.command,nonIndexed.handle,nonOffset,2,32);
        vkCmdBindIndexBuffer(context.command,indices.handle,16,VK_INDEX_TYPE_UINT32);
        prepare(160,{3,26},{5,7},{-4,6},2); vkCmdDrawIndexedIndirect(context.command,indirect.handle,indexedOffset,2,32);
    }
    vkCmdEndRenderPass(context.command); context.finish(); finishReplayFrame();
    printCacheStatistics(context);
    return checkRows(output,expected,fans?"triangle-fan-system-values":"draw-system-values");
}

static bool compute(const char* runtime,const char* unused,bool emptySets=false, bool tail=false, bool lateCache=false) {
    Context context(emptySets);
    // Match renderer startup: an internal helper can exist before the first
    // title's ShaderInfo provides the default persistence directory.
    if(lateCache)context.compute(context.shader(unused));
    configureCache(context);
    auto active=context.compute(context.shader(runtime)), inactive=context.compute(context.shader(unused));
    const uint32_t total=128;
    Buffer output=context.buffer(total*sizeof(Row),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); context.output(output,total*sizeof(Row));
    const uint32_t indirectOffset=tail?4:16;
    Buffer indirect=context.buffer(tail?indirectOffset+12:64,VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::vector<Row> expected(total,Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});
    context.begin();
    uint32_t groups[]={2,2,2}; vkCmdUpdateBuffer(context.command,indirect.handle,indirectOffset,sizeof(groups),groups);
    context.barrier(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,VK_ACCESS_INDIRECT_COMMAND_READ_BIT);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
    auto prepare=[&](uint32_t destination,std::array<uint32_t,3> origin) {
        uint32_t push[8]={destination,0,0,0,origin[0],origin[1],origin[2],0};
        vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        for(uint32_t z=0;z<2;++z) for(uint32_t y=0;y<2;++y) for(uint32_t x=0;x<2;++x) {
            uint32_t slot=destination+((z*2+y)*2+x)*2;
            expected[slot]={origin[0]+x,origin[1]+y,origin[2]+z,2};
            expected[slot+1]={2,2,origin[0]+x,0xcafe};
        }
    };
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,active);
    prepare(0,{3,2,1}); vkCmdDispatchBase(context.command,3,2,1,2,2,2);
    prepare(32,{0,0,0}); vkCmdDispatch(context.command,2,2,2);
    prepare(64,{0,0,0}); vkCmdDispatchIndirect(context.command,indirect.handle,indirectOffset);
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,inactive);
    uint32_t push[8]={96,0,0,0,0,0,0,0}; vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
    expected[96]={11,22,33,44}; vkCmdDispatch(context.command,1,1,1);
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,active);
    prepare(100,{5,1,3}); vkCmdDispatchBase(context.command,5,1,3,2,2,2);
    context.finish(); finishReplayFrame(); printCacheStatistics(context);
    return checkRows(output,expected,"dispatch-system-values-and-switch");
}

int main(int argc,char** argv) {
    if(argc!=4&&argc!=5) return 64;
    cacheDirectory=argc==5?argv[4]:nullptr;
    alarm(60);
    try {
        if (std::string(argv[1]) == "toggle") {
            const char* first = getenv("MELONX_EXPERIMENTAL_METAL_IR");
            bool initialIR = first && !strcmp(first, "1");
            bool success = graphics(argv[2],argv[3],false);
            setenv("MELONX_EXPERIMENTAL_METAL_IR", initialIR ? "0" : "1", 1);
            success = graphics(argv[2],argv[3],false) && success;
            setenv("MELONX_EXPERIMENTAL_METAL_IR", initialIR ? "1" : "0", 1);
            success = graphics(argv[2],argv[3],false) && success;
            return success ? 0 : 1;
        }
        const std::string mode=argv[1];
        bool success=(mode=="compute" || mode=="empty" || mode=="compute-tail" || mode=="compute-late")
            ?compute(argv[2],argv[3],mode=="empty",mode=="compute-tail",mode=="compute-late")
            :graphics(argv[2],argv[3],mode=="fan" || mode=="fan-indirect",mode=="tail",mode=="fan-indirect");
        return success?0:1;
    } catch(const std::exception& error) { fprintf(stderr,"probe failed: %s\n",error.what()); return 1; }
}
