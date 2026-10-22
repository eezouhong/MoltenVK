#pragma once
#include "MVKMetalIRBridge.h"

// Accepted pipeline inputs, separate from Vulkan feature advertising. Refactoring
// this policy must not implicitly change advertised features or add fallback.
struct MVKShaderRouteCapabilities {
    bool metalIR = false;
    bool supportsSpecialization(uint32_t entries) const { return !metalIR || entries == 0; }
    bool supportsStageClass(bool vertexFragmentCompute) const { return !metalIR || vertexFragmentCompute; }
    bool supportsPushDescriptors() const { return !metalIR; }
    // Mesh shaders compile to Metal mesh functions; tessellation has no IR route.
    bool supportsStages(bool tessellation, bool /*mesh*/) const { return !metalIR || !tessellation; }
    bool supportsGraphics(bool tessellation, bool mesh, bool discard, bool multiview) const {
        // Discard suppresses rasterization, not vertex-stage storage writes. A
        // mesh function has no void-output variant, so it must rasterize.
        return !metalIR || (!tessellation && !multiview && !(mesh && discard));
    }
    // Deterministic diagnostic classification only; supportsGraphics stays authoritative.
    const char* graphicsRejectionReason(bool tessellation, bool mesh, bool discard, bool multiview) const {
        if (!metalIR) return nullptr;
        if (tessellation) return "tessellation_stage";
        if (multiview) return "multiview";
        if (mesh && discard) return "mesh_rasterization_disabled";
        return nullptr;
    }
    bool supportsSampleShading(bool enabled, float minimum) const { return !metalIR || !enabled || minimum == 0.0f; }
    bool supportsVertexBinding(uint32_t index) const { return !metalIR || index > 2; }
    bool supportsRuntimeVertexBinding(uint32_t index) const { return !metalIR || (index != 4 && index != 5); }
    bool supportsPointSize(uint32_t flags) const { return !metalIR || (flags & (MVK_METAL_IR_UNIT_POINT_SIZE | MVK_METAL_IR_NATIVE_POINT_SIZE)); }
    bool supportsPointCoordinates(uint32_t flags) const { return !metalIR || (flags & MVK_METAL_IR_NATIVE_POINT_COORDINATES); }
    bool supportsDescriptorAllocation(bool empty, bool metal3, bool cpuVariable, bool gpuVariable, bool dynamicOffsets) const {
        return !metalIR || ((empty || metal3) && !cpuVariable && !gpuVariable && !dynamicOffsets);
    }
    bool supportsDescriptorType(bool unsupportedKind, bool multiPlane) const { return !metalIR || (!unsupportedKind && !multiPlane); }
    bool supportsDescriptorLayout(bool empty, bool metal3, bool cpuVariable, bool gpuVariable,
                                  bool dynamicOffsets, bool hasTable) const {
        return !metalIR || empty || (metal3 && !cpuVariable && !gpuVariable && !dynamicOffsets && hasTable);
    }
    bool supportsDescriptorSets(uint32_t count) const { return !metalIR || count <= 8; }
};
