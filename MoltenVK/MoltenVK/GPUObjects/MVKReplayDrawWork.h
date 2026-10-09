// API input counts, not hardware shader invocation statistics.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <limits>

namespace mvkreplay {
struct GPUDrawWork {
    uint64_t vertexProgram=0,fragmentProgram=0,sequenceHash=1469598103934665603ULL;
    uint64_t draws=0,inputElements=0,indexedDraws=0,unknownDraws=0;
    bool mixedPrograms=false,overflow=false;
    void add(uint64_t vertex,uint64_t fragment,uint64_t elements,uint64_t instances,bool indexed,bool known) {
        if(!draws){vertexProgram=vertex;fragmentProgram=fragment;}
        else mixedPrograms|=vertexProgram!=vertex||fragmentProgram!=fragment;
        ++draws;indexedDraws+=indexed;
        for(auto value:{vertex,fragment,elements,instances,uint64_t(indexed),uint64_t(known)}) {
            sequenceHash^=value;sequenceHash*=1099511628211ULL;
        }
        if(!known){++unknownDraws;return;}
        if(instances&&elements>std::numeric_limits<uint64_t>::max()/instances){overflow=true;return;}
        uint64_t count=elements*instances;
        if(inputElements>std::numeric_limits<uint64_t>::max()-count){overflow=true;return;}
        inputElements+=count;
    }
};
}
