#include "vulkan_context.h"

class DescriptorFixture : public Context {
public:
    VkSampler repeat{}, clamp{};
    VkImageView swapped{};
    VkDescriptorSetLayout layouts[3]{};
    VkPipelineLayout pipelineLayouts[3]{};
    VkDescriptorSet sets[3]{};
    VkPipeline programs[3]{};
    Buffer result{};

    explicit DescriptorFixture(const char* shaderFile) {
        vkDestroyPipelineLayout(device, layout, nullptr); layout = VK_NULL_HANDLE;
        vkDestroyDescriptorPool(device, descriptorPool, nullptr); descriptorPool = VK_NULL_HANDLE;
        vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr); descriptorLayout = VK_NULL_HANDLE;
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
        sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VK_CHECK(vkCreateSampler(device,&sampler,nullptr,&repeat));
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(device,&sampler,nullptr,&clamp));
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType=VK_IMAGE_TYPE_2D;
        info.format=VK_FORMAT_R8G8B8A8_UNORM; info.extent={4,1,1}; info.mipLevels=info.arrayLayers=1;
        info.samples=VK_SAMPLE_COUNT_1_BIT; info.tiling=VK_IMAGE_TILING_OPTIMAL;
        info.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        VK_CHECK(vkCreateImage(device,&info,nullptr,&image));
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device,image,&requirements);
        imageMemory=allocate(requirements,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkBindImageMemory(device,image,imageMemory,0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image;
        vi.format=info.format; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VK_CHECK(vkCreateImageView(device,&vi,nullptr,&view));
        vi.components={VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_A};
        VK_CHECK(vkCreateImageView(device,&vi,nullptr,&swapped));
        Buffer upload=buffer(16,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        const uint8_t pixels[]={0,0,255,255, 0,255,0,255, 255,255,255,255, 255,0,0,255};
        memcpy(upload.mapped,pixels,sizeof(pixels));
        fprintf(stderr,"descriptor upload begin\n");
        begin();
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.image=image; barrier.subresourceRange=vi.subresourceRange;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={4,1,1};
        vkCmdCopyBufferToImage(command,upload.handle,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        finish(); VK_CHECK(vkResetCommandPool(device,commandPool,0));
        fprintf(stderr,"descriptor upload complete\n");

        VkSampler fixed[]={clamp,clamp,clamp};
        for(unsigned i=0;i<3;++i) {
            VkDescriptorSetLayoutBinding bindings[]={
                {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,i==2?3u:2u,VK_SHADER_STAGE_COMPUTE_BIT,i==1?fixed:nullptr},
                {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,0,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
                {2,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2,VK_SHADER_STAGE_COMPUTE_BIT,i==1?fixed:nullptr},
                {3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
                {4,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
                {5,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
                {6,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,fixed}};
            VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            dl.bindingCount=7; dl.pBindings=bindings; VK_CHECK(vkCreateDescriptorSetLayout(device,&dl,nullptr,&layouts[i]));
            VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
            VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            pl.setLayoutCount=1; pl.pSetLayouts=&layouts[i]; pl.pushConstantRangeCount=1; pl.pPushConstantRanges=&push;
            VK_CHECK(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayouts[i]));
        }
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,13},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3},{VK_DESCRIPTOR_TYPE_SAMPLER,6},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,3}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets=3; pool.poolSizeCount=4; pool.pPoolSizes=sizes;
        VK_CHECK(vkCreateDescriptorPool(device,&pool,nullptr,&descriptorPool));
        VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocateInfo.descriptorPool=descriptorPool; allocateInfo.descriptorSetCount=3; allocateInfo.pSetLayouts=layouts;
        VK_CHECK(vkAllocateDescriptorSets(device,&allocateInfo,sets));
        result=buffer(40*sizeof(Row),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        VkDescriptorBufferInfo output{result.handle,0,40*sizeof(Row)};
        VkWriteDescriptorSet writes[3]{};
        for(unsigned i=0;i<3;++i) {
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=sets[i]; writes[i].dstBinding=3;
            writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[i].pBufferInfo=&output;
        }
        vkUpdateDescriptorSets(device,3,writes,0,nullptr);
        VkDescriptorImageInfo separate{repeat,swapped,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        for(unsigned i=0;i<3;++i) {
            VkWriteDescriptorSet updates[2]{};
            for(unsigned j=0;j<2;++j) {
                updates[j]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; updates[j].dstSet=sets[i];
                updates[j].dstBinding=4+j; updates[j].descriptorCount=1; updates[j].pImageInfo=&separate;
                updates[j].descriptorType=j?VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:VK_DESCRIPTOR_TYPE_SAMPLER;
            }
            vkUpdateDescriptorSets(device,2,updates,0,nullptr);
        }
        auto module=shader(shaderFile);
        for(unsigned i=0;i<3;++i) { layout=pipelineLayouts[i]; programs[i]=compute(module); }
        layout=VK_NULL_HANDLE; // Owned by this fixture, not the base context.
    }
    VkDescriptorImageInfo imageInfo(bool swap=false) const { return {repeat,swap?swapped:view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}; }
    void write(unsigned set,unsigned first,const std::vector<VkDescriptorImageInfo>& images) {
        VkWriteDescriptorSet update{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; update.dstSet=sets[set];
        update.dstBinding=0; update.dstArrayElement=first; update.descriptorCount=uint32_t(images.size());
        update.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; update.pImageInfo=images.data();
        vkUpdateDescriptorSets(device,1,&update,0,nullptr);
    }
    void copy(unsigned source,unsigned destination,unsigned dstFirst=0) {
        VkCopyDescriptorSet update{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
        update.srcSet=sets[source]; update.dstSet=sets[destination]; update.dstArrayElement=dstFirst; update.descriptorCount=4;
        vkUpdateDescriptorSets(device,0,nullptr,1,&update);
    }
    void run(unsigned set,uint32_t destination) {
        fprintf(stderr,"descriptor dispatch set=%u destination=%u\n",set,destination);
        begin(); vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,programs[set]);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayouts[set],0,1,&sets[set],0,nullptr);
        vkCmdPushConstants(command,pipelineLayouts[set],VK_SHADER_STAGE_COMPUTE_BIT,0,4,&destination);
        vkCmdDispatch(command,1,1,1); finish(); VK_CHECK(vkResetCommandPool(device,commandPool,0));
    }
    ~DescriptorFixture() {
        vkDeviceWaitIdle(device); vkResetCommandPool(device,commandPool,0);
        for(auto p:pipelines) vkDestroyPipeline(device,p,nullptr); pipelines.clear();
        for(auto p:pipelineLayouts) vkDestroyPipelineLayout(device,p,nullptr);
        vkDestroyDescriptorPool(device,descriptorPool,nullptr); descriptorPool=VK_NULL_HANDLE;
        for(auto p:layouts) vkDestroyDescriptorSetLayout(device,p,nullptr);
        vkDestroyImageView(device,swapped,nullptr);
        vkDestroySampler(device,repeat,nullptr); vkDestroySampler(device,clamp,nullptr);
    }
};

int main(int argc,char** argv) {
    if(argc!=2) return 64; alarm(60);
    try {
        DescriptorFixture f(argv[1]);
        const Row red{255,0,0,255},green{0,255,0,255},untouched{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd};
        std::vector<Row> expected(40,untouched);
        auto expect=[&](unsigned offset,std::array<Row,4> rows, bool copiedSampler=false){for(unsigned i=0;i<4;++i)expected[offset+i]=rows[i];expected[offset+4]=copiedSampler?green:red;expected[offset+5]=green;};
        f.write(0,0,{f.imageInfo()});
        f.write(0,1,{f.imageInfo(true),f.imageInfo(),f.imageInfo(true)});
        f.run(0,0); expect(0,{green,red,green,red});
        f.copy(0,1); f.run(1,6); expect(6,{red,green,red,green});
        f.copy(1,0); f.run(0,12); expect(12,{red,green,red,green});
        VkCopyDescriptorSet samplerCopy{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
        samplerCopy.srcSet=samplerCopy.dstSet=f.sets[0]; samplerCopy.srcBinding=6; samplerCopy.dstBinding=4; samplerCopy.descriptorCount=1;
        vkUpdateDescriptorSets(f.device,0,nullptr,1,&samplerCopy);
        VkDescriptorUpdateTemplateEntry entry{0,1,3,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,0,sizeof(VkDescriptorImageInfo)};
        VkDescriptorUpdateTemplateCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO};
        ti.descriptorUpdateEntryCount=1; ti.pDescriptorUpdateEntries=&entry;
        ti.templateType=VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET; ti.descriptorSetLayout=f.layouts[0];
        VkDescriptorUpdateTemplate updateTemplate; VK_CHECK(vkCreateDescriptorUpdateTemplate(f.device,&ti,nullptr,&updateTemplate));
        VkDescriptorImageInfo updates[]={f.imageInfo(),f.imageInfo(true),f.imageInfo()};
        vkUpdateDescriptorSetWithTemplate(f.device,f.sets[0],updateTemplate,updates);
        vkDestroyDescriptorUpdateTemplate(f.device,updateTemplate,nullptr);
        f.run(0,18); expect(18,{red,green,red,green},true);
        f.write(2,0,{f.imageInfo(),f.imageInfo(),f.imageInfo(),f.imageInfo(),f.imageInfo()});
        f.copy(1,2,1); f.run(2,24); expect(24,{green,red,red,green});
        VkDescriptorImageInfo samplerInfo{f.repeat,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
        VkWriteDescriptorSet samplerWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; samplerWrite.dstSet=f.sets[0];
        samplerWrite.dstBinding=4; samplerWrite.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER; samplerWrite.descriptorCount=1; samplerWrite.pImageInfo=&samplerInfo;
        vkUpdateDescriptorSets(f.device,1,&samplerWrite,0,nullptr);
        f.run(0,30); expect(30,{red,green,red,green});
        return checkRows(f.result,expected,"descriptor-write-copy-template-immutable-view")?0:1;
    } catch(const std::exception& error) { fprintf(stderr,"descriptor fixture: %s\n",error.what()); return 1; }
}
