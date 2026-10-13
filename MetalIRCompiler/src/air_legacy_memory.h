#pragma once
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/LLVMBitCodes.h>
#include <llvm/Support/ModRef.h>

namespace melonx::air {
// AIR's reader accepts the older attribute encoding. Keep the exact memory
// effects instead of dropping optimizer information during serialization.
struct LegacyMemory {
    llvm::SmallVector<unsigned,3> attributes;
    bool exact;
};
inline LegacyMemory legacyMemory(llvm::MemoryEffects effects) {
    using namespace llvm;
    LegacyMemory result;
    auto decoded=MemoryEffects::unknown();
    auto add=[&](unsigned code,MemoryEffects constraint) {
        result.attributes.push_back(code);decoded&=constraint;
    };
    if (effects.doesNotAccessMemory()) {
        add(bitc::ATTR_KIND_READ_NONE,MemoryEffects::none());
    } else {
        if (effects.onlyReadsMemory()) add(bitc::ATTR_KIND_READ_ONLY,MemoryEffects::readOnly());
        if (effects.onlyWritesMemory()) add(bitc::ATTR_KIND_WRITEONLY,MemoryEffects::writeOnly());
        if (effects.onlyAccessesArgPointees()) add(bitc::ATTR_KIND_ARGMEMONLY,MemoryEffects::argMemOnly());
        else if (effects.onlyAccessesInaccessibleMem()) add(bitc::ATTR_KIND_INACCESSIBLEMEM_ONLY,MemoryEffects::inaccessibleMemOnly());
        else if (effects.onlyAccessesInaccessibleOrArgMem()) add(bitc::ATTR_KIND_INACCESSIBLEMEM_OR_ARGMEMONLY,MemoryEffects::inaccessibleOrArgMemOnly());
    }
    result.exact=decoded==effects;
    return result;
}
}
