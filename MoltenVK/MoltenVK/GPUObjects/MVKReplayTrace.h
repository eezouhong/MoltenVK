// Diagnostic interface; opt-in implementation follows in the diagnostics commit.
#pragma once
#include <cstdint>
namespace mvkreplay {
enum Region : uint32_t { SPIRVToMSL,MSLLibrary,MSLFunction,IRLibrary,IRFunction,IRReflection,MetalGraphicsPSO,MetalComputePSO,DescriptorUpdate,IRShadowUpdate,DescriptorBinding,IRRootBinding,ParameterCopy,IndirectParameters,MetalCommandEncoding,RegionCount };
class Timer {public: explicit Timer(Region) {}};
inline void indirectRuntime(uint32_t,uint64_t,bool,bool) {}
inline void directRuntimeUpload() {}
}
