#include "MVKShaderBackend.h"
#include "MVKMetalIR.h"

namespace {
class MSLShaderBackend final : public MVKShaderBackend {
public:
    MSLShaderBackend() : MVKShaderBackend(false) {}
    MVKShaderStageResult compileStage(const MVKShaderStageRequest& request) const override {
        MVKShaderStageResult result;
        result.function = request.compileMSL();
        return result;
    }
    bool addGraphicsStages(const std::function<bool()>& msl, const std::function<bool()>&) const override { return msl(); }
    MVKMTLFunction computeFunction(const std::function<MVKMTLFunction()>& msl, const std::function<MVKMTLFunction()>&) const override { return msl(); }
};
class MetalIRShaderBackend final : public MVKShaderBackend {
public:
    MetalIRShaderBackend() : MVKShaderBackend(true) {}
    MVKShaderStageResult compileStage(const MVKShaderStageRequest& request) const override {
        MVKShaderStageResult result;
        result.artifactOwner = mvkCompileMetalIR(request.owner, request.layout, request.module,
                                                request.stage, request.vertexTransforms, request.runtimeOptions,
                                                request.dropFragDepth, request.dropFragStencilRef);
        if (auto& artifact = result.artifactOwner) {
            result.metadata = artifact->metadata;
            result.function = MVKMTLFunction(artifact->function, {},
                MTLSizeMake(artifact->threadgroupSize[0], artifact->threadgroupSize[1], artifact->threadgroupSize[2]));
        }
        return result;
    }
    bool addGraphicsStages(const std::function<bool()>&, const std::function<bool()>& ir) const override { return ir(); }
    MVKMTLFunction computeFunction(const std::function<MVKMTLFunction()>&, const std::function<MVKMTLFunction()>& ir) const override { return ir(); }
};
}
MVKShaderBackend* MVKShaderBackend::create(bool ir) {
    return ir ? static_cast<MVKShaderBackend*>(new MetalIRShaderBackend) : new MSLShaderBackend;
}
