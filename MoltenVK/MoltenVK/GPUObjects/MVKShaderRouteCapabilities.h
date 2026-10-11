#pragma once
#include "MVKMetalIRBridge.h"

// Accepted pipeline inputs, separate from Vulkan feature advertising. Refactoring
// this policy must not implicitly change advertised features or add fallback.
struct MVKShaderRouteCapabilities {
    bool metalIR = false;
    bool supportsPushDescriptors() const { return !metalIR; }
    bool supportsStages(bool tessellation, bool mesh) const { return !metalIR || (!tessellation && !mesh); }
    bool supportsGraphics(bool tessellation, bool mesh, bool discard, bool multiview) const {
        return !metalIR || (!tessellation && !mesh && !discard && !multiview);
    }
    bool supportsSampleShading(bool enabled, float minimum) const { return !metalIR || !enabled || minimum == 0.0f; }
    bool supportsVertexBinding(uint32_t index) const { return !metalIR || index > 2; }
    bool supportsRuntimeVertexBinding(uint32_t index) const { return !metalIR || (index != 4 && index != 5); }
    bool supportsPointSize(uint32_t flags) const { return !metalIR || (flags & (MVK_METAL_IR_UNIT_POINT_SIZE | MVK_METAL_IR_NATIVE_POINT_SIZE)); }
    bool supportsPointCoordinates(uint32_t flags) const { return !metalIR || (flags & MVK_METAL_IR_NATIVE_POINT_COORDINATES); }
    bool supportsDescriptorLayout(bool empty, bool metal3, bool cpuVariable, bool gpuVariable,
                                  bool dynamicOffsets, bool hasTable) const {
        return !metalIR || empty || (metal3 && !cpuVariable && !gpuVariable && !dynamicOffsets && hasTable);
    }
    bool supportsDescriptorSets(uint32_t count) const { return !metalIR || count <= 8; }
};
