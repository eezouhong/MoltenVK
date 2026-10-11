#pragma once
#include "MVKShaderModule.h"
#include "MVKShaderRouteCapabilities.h"
#include "MVKLibraryResidency.h"
#include <functional>
#include <memory>

class MVKPipeline;
class MVKPipelineLayout;
struct MVKMetalIRArtifact;
struct MVKMetalIRMetadata;

struct MVKShaderStageRequest {
    MVKPipeline* owner;
    MVKPipelineLayout* layout;
    MVKShaderModule* module;
    const VkPipelineShaderStageCreateInfo* stage;
    uint32_t vertexTransforms = 0;
    uint32_t runtimeOptions = 0;
    // Wrap the existing conversion/cache logic; its configuration names and
    // mutations deliberately remain unchanged during this structural step.
    std::function<MVKMTLFunction()> compileMSL;
};
struct MVKShaderStageResult {
    // Carries existing reflection and optional MTL4 descriptor/keys unchanged.
    MVKMTLFunction function = MVKMTLFunctionNull;
    std::shared_ptr<const MVKMetalIRMetadata> metadata;
    std::shared_ptr<MVKMetalIRArtifact> artifactOwner;
};

class MVKShaderBackend {
public:
    explicit MVKShaderBackend(bool ir) : _capabilities{ir}, _residency(MVKLibraryResidency::create(ir)) {}
    virtual ~MVKShaderBackend() = default;
    const MVKShaderRouteCapabilities& capabilities() const { return _capabilities; }
    MVKLibraryResidency& libraryResidency() const { return *_residency; }
    virtual MVKShaderStageResult compileStage(const MVKShaderStageRequest&) const = 0;
    virtual bool addGraphicsStages(const std::function<bool()>& msl,
                                   const std::function<bool()>& ir) const = 0;
    virtual MVKMTLFunction computeFunction(const std::function<MVKMTLFunction()>& msl,
                                         const std::function<MVKMTLFunction()>& ir) const = 0;
    static MVKShaderBackend* create(bool ir);
private:
    const MVKShaderRouteCapabilities _capabilities;
    const std::unique_ptr<MVKLibraryResidency> _residency;
};
