// Pure data validation for optional GPU stage observations.
#pragma once
#include "MVKReplayFrameTrace.h"
#include <array>
#include <limits>
#include <memory>
#include <unordered_map>

namespace mvkreplay {
enum GPUStageUnavailable : uint32_t {
    StageUnsupported = 1, StagePoolFull = 2, StagePassLimit = 4,
    StageAttachmentUsed = 8, StageResolveFailed = 16, StageInvalidTimestamp = 32,
    StageInvalidClock = 64, StageRegistryFull = 128
};
inline bool sampleGPUStageEpoch(uint64_t frame,uint64_t seed) {
    if(frame==1)return true; // deterministic headless-probe startup, excluded from game sampling
    uint64_t value=frame+seed+0x9e3779b97f4a7c15ULL;
    value=(value^(value>>30))*0xbf58476d1ce4e5b9ULL;
    value=(value^(value>>27))*0x94d049bb133111ebULL;
    return ((value^(value>>31))&15u)==0;
}
class StageSlotAllocator {
    uint32_t used = 0;
public:
    static constexpr unsigned capacity = 8;
    std::optional<unsigned> acquire() {
        for (unsigned i = 0; i < capacity; ++i) {
            if (used & (1u << i)) continue;
            used |= 1u << i; return i;
        }
        return {};
    }
    bool release(unsigned slot) {
        if (slot >= capacity || !(used & (1u << slot))) return false;
        used &= ~(1u << slot); return true;
    }
};
struct GPUStageEpoch {
    uint64_t frame=0,outstanding=0;
    uint32_t nextEncoder=0;
    bool sealed=false;
    std::optional<unsigned> slot;
};
// External serialization. Sealing may race a begin that already has its frame
// token; remember the high-water mark so a late begin cannot leak a slot.
class GPUStageEpochTracker {
    StageSlotAllocator slots;
    std::unordered_map<uint64_t,std::shared_ptr<GPUStageEpoch>> epochs;
    uint64_t sealedThrough=0;
    void retire(const std::shared_ptr<GPUStageEpoch>& epoch) {
        if(!epoch->sealed||epoch->outstanding)return;
        if(epoch->slot)slots.release(*epoch->slot);
        epochs.erase(epoch->frame);
    }
public:
    struct Allocation { unsigned slot,index; };
    std::shared_ptr<GPUStageEpoch> begin(uint64_t frame) {
        auto i=epochs.find(frame);
        if(i==epochs.end()) {
            if(epochs.size()>=StageSlotAllocator::capacity)return {};
            auto epoch=std::make_shared<GPUStageEpoch>();epoch->frame=frame;
            epoch->sealed=frame<=sealedThrough;i=epochs.emplace(frame,epoch).first;
        }
        ++i->second->outstanding;return i->second;
    }
    std::optional<Allocation> reserve(const std::shared_ptr<GPUStageEpoch>& epoch) {
        if(!epoch||epoch->nextEncoder>=1024)return {};
        if(!epoch->slot)epoch->slot=slots.acquire();
        if(!epoch->slot)return {};
        return Allocation{*epoch->slot,4*epoch->nextEncoder++};
    }
    bool complete(const std::shared_ptr<GPUStageEpoch>& epoch) {
        if(!epoch||!epoch->outstanding)return false;
        --epoch->outstanding;retire(epoch);return true;
    }
    void seal(uint64_t frame) {
        sealedThrough=std::max(sealedThrough,frame);
        std::vector<std::shared_ptr<GPUStageEpoch>> ready;
        for(auto& pair:epochs)if(pair.first<=sealedThrough) {pair.second->sealed=true;ready.push_back(pair.second);}
        for(auto& epoch:ready)retire(epoch);
    }
    size_t active() const { return epochs.size(); }
};
struct GPUStageDurations {
    uint64_t vertexTicks = 0, fragmentTicks = 0, computeTicks = 0, blitTicks = 0, unionTicks = 0;
    uint32_t validPasses = 0, invalidPasses = 0, emptyStages = 0;
};
inline GPUStageDurations stageDurations(const uint64_t* values, const uint8_t* kinds, size_t passes,
                                        uint64_t lower=0, uint64_t upper=std::numeric_limits<uint64_t>::max(), const bool* encodedDraw=nullptr) {
    std::vector<GPUInterval> vertex, fragment, compute, blit, all;
    GPUStageDurations result;
    for (size_t i = 0; i < passes; ++i) {
        const auto* t = values + i * 4;
        bool valid=kinds[i]<=2;
        auto pair=[&](unsigned index,std::vector<GPUInterval>& group) {
            uint64_t begin=t[index],end=t[index+1];
            bool current=begin&&end>=begin&&begin>=lower&&end<=upper&&end!=std::numeric_limits<uint64_t>::max();
            if(current){group.push_back({begin,end});all.push_back(group.back());return;}
            bool knownEmpty=kinds[i]==0&&encodedDraw&&!encodedDraw[i];
            bool unwritten=(!begin&&!end)||(begin<lower&&end<lower);
            if(knownEmpty&&unwritten){++result.emptyStages;return;}
            valid=false;
        };
        if(kinds[i]==0){pair(0,vertex);pair(2,fragment);}
        else if(kinds[i]==1)pair(0,compute);
        else if(kinds[i]==2)pair(0,blit);
        if(valid)++result.validPasses;else++result.invalidPasses;
    }
    result.vertexTicks = gpuIntervalUnion(std::move(vertex));
    result.fragmentTicks = gpuIntervalUnion(std::move(fragment));
    result.computeTicks = gpuIntervalUnion(std::move(compute));
    result.blitTicks = gpuIntervalUnion(std::move(blit));
    result.unionTicks = gpuIntervalUnion(std::move(all));
    return result;
}
struct GPUComputeWork {
    uint64_t firstProgram=0,sequenceHash=1469598103934665603ULL;
    uint64_t dispatches=0,groups=0,invocations=0,indirect=0;
    bool mixedPrograms=false,overflow=false;
    void add(uint64_t program,uint64_t x,uint64_t y,uint64_t z,
             uint64_t tx,uint64_t ty,uint64_t tz,bool isIndirect) {
        if(!dispatches)firstProgram=program;else mixedPrograms|=firstProgram!=program;
        ++dispatches;
        for(auto value:{program,x,y,z,tx,ty,tz,uint64_t(isIndirect)}) {sequenceHash^=value;sequenceHash*=1099511628211ULL;}
        if(isIndirect){++indirect;return;}
        auto multiply=[&](uint64_t a,uint64_t b) {
            if(b&&a>std::numeric_limits<uint64_t>::max()/b){overflow=true;return uint64_t(0);}return a*b;
        };
        auto accumulate=[&](uint64_t& total,uint64_t value) {
            if(total>std::numeric_limits<uint64_t>::max()-value){overflow=true;return;}total+=value;
        };
        uint64_t count=multiply(multiply(x,y),z);
        accumulate(groups,count);accumulate(invocations,multiply(count,multiply(multiply(tx,ty),tz)));
    }
};
struct GPUStageClock {
    uint64_t hostMidNs = 0, hostSpanNs = 0, cpuTicks = 0, gpuTicks = 0;
};
inline std::optional<double> stageClockScale(const GPUStageClock& a, const GPUStageClock& b) {
    if (!a.gpuTicks || b.gpuTicks <= a.gpuTicks || b.hostMidNs <= a.hostMidNs) return {};
    uint64_t hostDelta = b.hostMidNs - a.hostMidNs;
    // Midpoint uncertainty must be <=1% of the calibration interval.
    if (static_cast<long double>(a.hostSpanNs) + b.hostSpanNs > hostDelta * .02L) return {};
    return double(hostDelta) / double(b.gpuTicks - a.gpuTicks);
}
}
