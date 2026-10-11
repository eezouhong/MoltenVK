#pragma once
#include "MVKFoundation.h"
#include <array>

class MVKDevice;
// Query/pressure boundary only. Adapters retain the existing route-specific
// algorithms, limits, protection and timing. See the version1 field contract.
class MVKLibraryResidency {
public:
    using Statistics = std::array<uint64_t,16>;
    virtual ~MVKLibraryResidency() = default;
    virtual Statistics statistics(MVKDevice*) const = 0;
    virtual VkResult trimPressure(MVKDevice*) const = 0;
    static MVKLibraryResidency* create(bool ir);
};
