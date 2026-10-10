#include "vulkan_context.h"
#include <dlfcn.h>
#include <unistd.h>
int main(int argc,char** argv){
 if(argc!=2)return 64;alarm(70);
 try{
  Context c;
  auto statistics=reinterpret_cast<uint32_t(*)(VkDevice,uint64_t*,uint32_t)>(dlsym(RTLD_DEFAULT,"vkGetMetalIRCompilerStatisticsMVK"));
  if(!statistics)throw std::runtime_error("statistics unavailable");
  uint64_t before[9]{},after[9]{};if(statistics(c.device,before,9)!=9)throw std::runtime_error("statistics fields unavailable");
  VkResult results[2]{};
  for(unsigned i=0;i<2;++i){
   // Two shader modules and two pipeline requests use the same stage bytes/key.
   // The shader is structurally valid SPIR-V; this is a rejection test for an
   // unsupported optional numeric feature, never a dispatch/correctness test.
   VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=c.layout;
   info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=c.shader(argv[1]);info.stage.pName="main";
   VkPipeline pipeline=VK_NULL_HANDLE;results[i]=vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&info,nullptr,&pipeline);
   if(pipeline)vkDestroyPipeline(c.device,pipeline,nullptr);
   if(statistics(c.device,after,9)!=9)throw std::runtime_error("statistics fields changed");
   printf("{\"attempt\":%u,\"vkResult\":%d,\"compilerCalls\":%llu,\"mscNs\":%llu,\"rejected\":%llu}\n",i+1,results[i],(unsigned long long)(after[0]-before[0]),(unsigned long long)(after[3]-before[3]),(unsigned long long)(after[7]-before[7]));fflush(stdout);
  }
  return 0; // Runner judges the observed MSC failure and negative-cache reuse.
 }catch(const std::exception& e){fprintf(stderr,"negative probe setup: %s\n",e.what());return 1;}
}
