#include "MVKLibraryResidency.h"
#include "MVKDevice.h"
#include "MVKShaderModule.h"
#include "MVKMetalIR.h"

namespace {
class IRResidency final : public MVKLibraryResidency {
public:
    Statistics statistics(MVKDevice* device) const override {
        Statistics result{};result[0]=1;result[2]=1;result[14]=1;
        uint64_t raw[37]{};
        if (mvkMetalIRCacheStatistics(device,raw,37)!=37 || !raw[3]) return result;
        result[1]=1;result[3]=raw[5];result[4]=raw[2];result[5]=raw[10];result[6]=raw[34];
        result[7]=raw[6];result[9]=raw[18];result[10]=raw[19];result[11]=raw[20];
        result[12]=raw[4];result[13]=1;result[15]=(raw[1]&4)!=0;
        return result;
    }
    VkResult trimPressure(MVKDevice*) const override {
        // Existing idle compiler-allocation relief only. No new clear/LRU/limit
        // policy is introduced under the name of structural unification.
        (void)mvkMetalIRRelieveCompilerMemory();
        return VK_SUCCESS;
    }
};
class MSLResidency final : public MVKLibraryResidency {
public:
    Statistics statistics(MVKDevice* device) const override {
        Statistics result{};result[0]=1;result[2]=2;result[14]=2;
        auto* repository=device->getShaderLibraryRepository();
        if (!repository) return result;
        MVKMetal4ShaderLibraryRepositoryStatistics raw{};repository->getMemoryStatistics(&raw);
        if (!raw.available) return result;
        result[1]=1;result[3]=raw.residentCanonicalShaderLibraryCount;result[4]=raw.residentLimit;
        result[5]=raw.residentEvictionCount;result[6]=raw.rehydrateCount;
        result[8]=raw.residentUncompressedMSLBytes;result[12]=raw.logicalMembershipCount;
        result[13]=2;result[15]=1;
        return result;
    }
    VkResult trimPressure(MVKDevice* device) const override {
        auto* repository=device->getShaderLibraryRepository();
        if (!repository) return VK_ERROR_FEATURE_NOT_PRESENT;
        repository->trimToResidentLimit();
        return VK_SUCCESS;
    }
};
}
MVKLibraryResidency* MVKLibraryResidency::create(bool ir) {
    return ir ? static_cast<MVKLibraryResidency*>(new IRResidency) : new MSLResidency;
}
