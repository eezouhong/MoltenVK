#include "vulkan_context.h"
#include <algorithm>
#include <cstdint>
#include <limits>

// Expected system values derive from the original Vulkan command, not indices
// or the conversion kernel. Sentinel rows expose unexpected shader writes.
int main(int argc, char** argv) {
    if (argc != 7) return 64;
    alarm(60);
    try {
        const std::string mode = argv[1];
        if (mode != "direct" && mode != "indirect") return 64;
        uint32_t first = std::stoul(argv[2]);
        uint32_t requested = std::stoul(argv[3]);
        uint32_t expectedCount = std::stoul(argv[4]);
        if (!requested || expectedCount > requested || requested > 200000) return 64;
        Context context; context.target();
        const size_t total = size_t(requested) * 2 + 8;
        Buffer output = context.buffer(total * sizeof(Row), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        context.output(output, total * sizeof(Row));
        auto pipeline = context.graphics(context.shader(argv[5]), context.shader(argv[6]), VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
        Buffer arguments = context.buffer(sizeof(VkDrawIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
        VkDrawIndirectCommand command{requested, 2, first, 7};
        memcpy(arguments.mapped, &command, sizeof(command));
        std::vector<Row> expected(total, Row{0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd,0xcdcdcdcd});
        for (uint32_t instance = 0; instance < 2; ++instance)
            for (uint32_t vertex = 0; vertex < expectedCount; ++vertex)
                expected[size_t(instance) * requested + vertex] = {first + vertex, first, 7 + instance, 7};
        context.begin();
        VkClearValue clear{};
        VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        begin.renderPass = context.renderPass; begin.framebuffer = context.framebuffer;
        begin.renderArea = {{0,0},{16,16}}; begin.clearValueCount = 1; begin.pClearValues = &clear;
        vkCmdBeginRenderPass(context.command, &begin, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(context.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_GRAPHICS,context.layout,0,1,&context.descriptor,0,nullptr);
        uint32_t push[8] = {first, requested, 7, 0, 0, 0, 0, 0};
        vkCmdPushConstants(context.command,context.layout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        if (mode == "direct") vkCmdDraw(context.command, requested, 2, first, 7);
        else vkCmdDrawIndirect(context.command, arguments.handle, 0, 1, sizeof(command));
        vkCmdEndRenderPass(context.command); context.finish();
        bool pass = checkRows(output, expected, mode.c_str());
        printf("{\"firstVertex\":%u,\"requestedVertexCount\":%u,\"expectedVertexCount\":%u,\"boundedConversionFixture\":%s}\n",
            first, requested, expectedCount, requested == expectedCount ? "false" : "true");
        return pass ? 0 : 1;
    } catch (const std::exception& error) { fprintf(stderr, "fan probe failed: %s\n", error.what()); return 1; }
}
