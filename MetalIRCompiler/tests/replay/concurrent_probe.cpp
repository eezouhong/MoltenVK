#include "vulkan_context.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

static double nowMs() {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Every module has a unique entry and output literal. This prevents resident
// cache deduplication from masquerading as successful concurrent compilation.
static std::vector<uint32_t> variant(const std::vector<uint32_t>& source,const std::string& entry,uint32_t value) {
    std::vector<uint32_t> result(source.begin(),source.begin()+5);
    uint32_t changedConstants=0,changedEntries=0;
    for(size_t pos=5;pos<source.size();) {
        uint32_t count=source[pos]>>16,op=source[pos]&65535;
        if(!count||pos+count>source.size())throw std::runtime_error("malformed fixture SPIR-V");
        if(op==15) { // OpEntryPoint: model, function, padded name, interfaces.
            size_t oldWords=0;
            for(size_t i=pos+3;i<pos+count;++i) {
                ++oldWords;if(memchr(&source[i],0,4))break;
            }
            std::vector<uint32_t> name((entry.size()+4)/4,0);memcpy(name.data(),entry.c_str(),entry.size());
            result.push_back(((count-oldWords+name.size())<<16)|op);
            result.push_back(source[pos+1]);result.push_back(source[pos+2]);
            result.insert(result.end(),name.begin(),name.end());
            result.insert(result.end(),source.begin()+pos+3+oldWords,source.begin()+pos+count);
            ++changedEntries;
        } else {
            size_t start=result.size();result.insert(result.end(),source.begin()+pos,source.begin()+pos+count);
            if(op==43&&count==4&&source[pos+3]==11) {result[start+3]=value;++changedConstants;}
        }
        pos+=count;
    }
    if(changedConstants!=1||changedEntries!=1)throw std::runtime_error("fixture lacks expected unique entry/literal");
    return result;
}

int main(int argc,char** argv) {
    if(argc!=5)return 64;
    alarm(120);
    try {
        unsigned workers=std::stoul(argv[2]),jobs=std::stoul(argv[3]),seed=std::stoul(argv[4]);
        if((workers!=1&&workers!=4)||jobs!=32||seed<100||seed>1000000000)return 64;
        std::ifstream file(argv[1],std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
        if(bytes.size()<20||bytes.size()%4)throw std::runtime_error("invalid input SPIR-V");
        std::vector<uint32_t> words(bytes.size()/4);memcpy(words.data(),bytes.data(),bytes.size());
        Context context;
        std::vector<VkShaderModule> modules(jobs);
        std::vector<std::string> names(jobs);
        std::vector<VkPipeline> pipelines(jobs,VK_NULL_HANDLE);
        std::vector<VkResult> status(jobs,VK_ERROR_UNKNOWN);
        std::vector<double> durations(jobs);
        for(unsigned i=0;i<jobs;++i) {
            names[i]="parallel_"+std::to_string(seed)+"_"+std::to_string(i);
            auto code=variant(words,names[i],seed+i);
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};info.codeSize=code.size()*4;info.pCode=code.data();
            VK_CHECK(vkCreateShaderModule(context.device,&info,nullptr,&modules[i]));context.modules.push_back(modules[i]);
        }
        std::mutex gate;std::condition_variable ready;unsigned arrived=0;bool released=false;
        std::atomic<unsigned> next{0},active{0},peak{0};std::vector<std::thread> threads;
        for(unsigned worker=0;worker<workers;++worker)threads.emplace_back([&] {
            {std::unique_lock<std::mutex> lock(gate);++arrived;ready.notify_all();ready.wait(lock,[&]{return released;});}
            for(;;) {
                unsigned i=next.fetch_add(1);if(i>=jobs)return;
                VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=context.layout;
                info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;
                info.stage.module=modules[i];info.stage.pName=names[i].c_str();
                unsigned concurrent=active.fetch_add(1)+1,old=peak.load();
                while(old<concurrent&&!peak.compare_exchange_weak(old,concurrent)){}
                double start=nowMs();status[i]=vkCreateComputePipelines(context.device,VK_NULL_HANDLE,1,&info,nullptr,&pipelines[i]);
                durations[i]=nowMs()-start;active.fetch_sub(1);
            }
        });
        double start;
        {std::unique_lock<std::mutex> lock(gate);ready.wait(lock,[&]{return arrived==workers;});start=nowMs();released=true;}ready.notify_all();
        for(auto& thread:threads)thread.join();double elapsed=nowMs()-start;
        for(unsigned i=0;i<jobs;++i) {
            if(pipelines[i])context.pipelines.push_back(pipelines[i]);
            VK_CHECK(status[i]);
        }
        Buffer output=context.buffer(64*sizeof(Row),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);context.output(output,64*sizeof(Row));
        std::vector<Row> expected(64,Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});
        context.begin();vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,context.layout,0,1,&context.descriptor,0,nullptr);
        for(unsigned i=0;i<jobs;++i) {
            uint32_t push[8]={i};
            vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[i]);
            vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
            vkCmdDispatch(context.command,1,1,1);expected[i]={seed+i,22,33,44};
        }
        context.finish();bool correct=checkRows(output,expected,"concurrent-pipeline-output");
        double total=0;for(double v:durations)total+=v;
        printf("{\"workers\":%u,\"jobs\":%u,\"seed\":%u,\"compileWallMs\":%.6f,\"sumRequestWallMs\":%.6f,\"throughputPerSecond\":%.6f,\"maxConcurrentRequests\":%u,\"success\":%s}\n",
               workers,jobs,seed,elapsed,total,jobs*1000.0/elapsed,peak.load(),correct?"true":"false");
        return correct?0:1;
    } catch(const std::exception& error) {fprintf(stderr,"concurrent probe: %s\n",error.what());return 1;}
}
