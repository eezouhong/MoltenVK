// Real Vulkan Loader regression. Calls the native virtual getter on live
// objects, then exercises command-buffer allocation lifetimes through Loader.
#include "vulkan_context.h"
#import <QuartzCore/CAMetalLayer.h>
#include "MVKVulkanAPIObject.h"
#include <cstring>

static bool preserved(void* handle) {
    uintptr_t before;memcpy(&before,handle,sizeof(before));
    auto* object=MVKDispatchableVulkanAPIObject::getDispatchableObject(handle);
    for(unsigned i=0;i<8;++i)if(object->getVkHandle()!=handle)return false;
    uintptr_t after;memcpy(&after,handle,sizeof(after));
    bool correct=before==after&&before!=ICD_LOADER_MAGIC;
    // Restore only in the failing test so Loader can clean up safely. Never
    // substitute this restoration for native production correctness.
    if(!correct)memcpy(handle,&before,sizeof(before));
    return correct;
}
int main() {
    alarm(30);
    try {
        Context context(false,false,false,false,false,false,true);
        // Loader owns separate wrappers for instance/physical handles. Device,
        // queue and command handles retain the driver's dispatchable storage.
        bool live=preserved(context.device)&&preserved(context.queue)&&preserved(context.command);
        if(!live) {puts("FAIL live native getter overwrote loader dispatch");return 1;}
        unsigned reused=0;
        for(unsigned i=0;i<32;++i) {
            VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            info.commandPool=context.commandPool;info.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;info.commandBufferCount=1;
            VkCommandBuffer command{};VK_CHECK(vkAllocateCommandBuffers(context.device,&info,&command));
            if(!preserved(command))return 1;
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            VK_CHECK(vkBeginCommandBuffer(command,&begin));VK_CHECK(vkEndCommandBuffer(command));
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
            VK_CHECK(vkQueueSubmit(context.queue,1,&submit,VK_NULL_HANDLE));VK_CHECK(vkQueueWaitIdle(context.queue));
            vkFreeCommandBuffers(context.device,context.commandPool,1,&command);++reused;
        }
        printf("PASS live device/queue/command getters and %u recycled allocations/submissions\n",reused);
        return 0;
    } catch(const std::exception& error) {fprintf(stderr,"loader probe: %s\n",error.what());return 1;}
}
