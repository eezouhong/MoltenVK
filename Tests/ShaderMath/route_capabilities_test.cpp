#include "MVKShaderRouteCapabilities.h"
#include <cassert>
#include <limits>
#include <initializer_list>
#include <cstring>

int main() {
    MVKShaderRouteCapabilities ir{true}, msl{false};
    for (unsigned mask=0; mask<64; ++mask) {
        bool tess=mask&1, mesh=mask&2, discard=mask&4, views=mask&8, sample=mask&16;
        for (float minimum : {0.0f, 0.5f, std::numeric_limits<float>::quiet_NaN()}) {
            assert(ir.supportsGraphics(tess,mesh,discard,views)==(!tess&&!mesh&&!discard&&!views));
            assert(ir.supportsSampleShading(sample,minimum)==!(sample&&minimum!=0.0f));
            assert(msl.supportsGraphics(tess,mesh,discard,views));
            assert((ir.graphicsRejectionReason(tess,mesh,discard,views) == nullptr) == ir.supportsGraphics(tess,mesh,discard,views));
            assert(msl.graphicsRejectionReason(tess,mesh,discard,views) == nullptr);
            assert(msl.supportsSampleShading(sample,minimum));
        }
        bool empty=mask&1, metal3=mask&2, cpu=mask&4, gpu=mask&8, dynamic=mask&16;
        assert(ir.supportsDescriptorAllocation(empty,metal3,cpu,gpu,dynamic)==((empty||metal3)&&!cpu&&!gpu&&!dynamic));
    }
    for (uint32_t index=0; index<32; ++index) {
        assert(ir.supportsVertexBinding(index)==(index>2));
        assert(ir.supportsRuntimeVertexBinding(index)==(index!=4&&index!=5));
        assert(msl.supportsVertexBinding(index));
    }
    for (uint32_t flags=0; flags<128; ++flags) {
        assert(ir.supportsPointSize(flags)==bool(flags&(MVK_METAL_IR_UNIT_POINT_SIZE|MVK_METAL_IR_NATIVE_POINT_SIZE)));
        assert(ir.supportsPointCoordinates(flags)==bool(flags&MVK_METAL_IR_NATIVE_POINT_COORDINATES));
    }
    assert(!ir.supportsDescriptorSets(9) && ir.supportsDescriptorSets(8));
    assert(std::strcmp(ir.graphicsRejectionReason(false,false,true,false), "rasterizer_discard") == 0);
    assert(std::strcmp(ir.graphicsRejectionReason(true,false,true,false), "tessellation_stage") == 0);
    assert(std::strcmp(ir.graphicsRejectionReason(false,true,false,false), "mesh_stage") == 0);
    assert(std::strcmp(ir.graphicsRejectionReason(false,false,false,true), "multiview") == 0);
    assert(!ir.supportsPushDescriptors() && msl.supportsPushDescriptors());
}
