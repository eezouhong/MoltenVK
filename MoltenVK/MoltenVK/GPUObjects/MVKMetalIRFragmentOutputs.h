#pragma once
#include "spirv.hpp"
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mvkir {
// Metal rejects a pipeline whose fragment function writes depth or stencil when that
// attachment is absent. The MSL route drops those builtins in SPIRV-Cross
// (enable_frag_depth_builtin / enable_frag_stencil_ref_builtin); do the same for the
// IR route by turning the FragDepth / FragStencilRefEXT output into a Private variable,
// so its stores stay valid and are discarded. Returns whether the module changed. A
// module is left alone when such a builtin is a block member or its pointer escapes
// into an instruction whose result type would also need rewriting.
inline bool dropFragmentOutputs(std::vector<uint32_t>& code, bool depth, bool stencil) {
    if((!depth && !stencil) || code.size()<5 || code[0]!=spv::MagicNumber)return false;
    struct Instruction { size_t pos; uint32_t op, count; };
    std::vector<Instruction> instructions;
    for(size_t pos=5;pos<code.size();) {
        uint32_t count=code[pos]>>16;
        if(!count || count>code.size()-pos)return false;
        instructions.push_back({pos,code[pos]&65535,count});pos+=count;
    }
    auto dropped=[&](uint32_t builtin) {
        return (depth && builtin==spv::BuiltInFragDepth) || (stencil && builtin==spv::BuiltInFragStencilRefEXT);
    };
    std::unordered_set<uint32_t> variables;
    for(const auto& in:instructions) {
        const uint32_t* w=code.data()+in.pos;
        if(in.op==spv::OpDecorate && in.count>=4 && w[2]==spv::DecorationBuiltIn && dropped(w[3]))variables.insert(w[1]);
        if(in.op==spv::OpMemberDecorate && in.count>=5 && w[3]==spv::DecorationBuiltIn && dropped(w[4]))return false;
    }
    if(variables.empty())return false;

    std::unordered_map<uint32_t,uint32_t> outputPointee, privatePointer;
    std::unordered_map<uint32_t,size_t> pointerAt;
    for(const auto& in:instructions) {
        const uint32_t* w=code.data()+in.pos;
        if(in.op==spv::OpTypePointer && in.count==4) {
            if(w[2]==spv::StorageClassOutput) { outputPointee[w[1]]=w[3];pointerAt[w[1]]=in.pos; }
            if(w[2]==spv::StorageClassPrivate && !privatePointer.count(w[3]))privatePointer[w[3]]=w[1];
        }
        if(in.op==spv::OpVariable && variables.count(w[2]) &&
           (in.count!=4 || w[3]!=spv::StorageClassOutput || !outputPointee.count(w[1])))return false;
        bool derives=in.op==spv::OpAccessChain || in.op==spv::OpInBoundsAccessChain || in.op==spv::OpPtrAccessChain ||
            in.op==spv::OpInBoundsPtrAccessChain || in.op==spv::OpCopyObject || in.op==spv::OpFunctionCall ||
            in.op==spv::OpPhi || in.op==spv::OpSelect;
        for(uint32_t i=3;derives && i<in.count;++i)if(variables.count(w[i]))return false;
    }

    // A missing Private pointer type goes right after the Output pointer it mirrors.
    uint32_t bound=code[3];
    std::unordered_map<size_t,std::vector<uint32_t>> inserts;
    std::unordered_map<uint32_t,uint32_t> variableType;
    for(const auto& in:instructions) {
        const uint32_t* w=code.data()+in.pos;
        if(in.op!=spv::OpVariable || !variables.count(w[2]))continue;
        uint32_t pointee=outputPointee[w[1]];
        if(!privatePointer.count(pointee)) {
            uint32_t id=bound++;privatePointer[pointee]=id;
            inserts[pointerAt[w[1]]].insert(inserts[pointerAt[w[1]]].end(),
                                            {(4u<<16)|spv::OpTypePointer,id,spv::StorageClassPrivate,pointee});
        }
        variableType[w[2]]=privatePointer[pointee];
    }
    // Before SPIR-V 1.4 the entry point interface lists only Input/Output variables.
    bool legacyInterface=code[1]<0x00010400;
    std::vector<uint32_t> result(code.begin(),code.begin()+5);
    result[3]=bound;
    for(const auto& in:instructions) {
        const uint32_t* w=code.data()+in.pos;
        if(in.op==spv::OpDecorate && in.count>=4 && w[2]==spv::DecorationBuiltIn && variables.count(w[1]))continue;
        if(in.op==spv::OpExecutionMode && in.count>=3) {
            uint32_t mode=w[2];
            if(depth && (mode==spv::ExecutionModeDepthReplacing || mode==spv::ExecutionModeDepthGreater ||
                         mode==spv::ExecutionModeDepthLess || mode==spv::ExecutionModeDepthUnchanged))continue;
            if(stencil && mode==spv::ExecutionModeStencilRefReplacingEXT)continue;
        }
        if(in.op==spv::OpEntryPoint && legacyInterface) {
            // The interface ids follow the name, whose last word holds its terminating zero byte.
            uint32_t i=3;
            while(i<in.count && (w[i]&0xFF) && (w[i]&0xFF00) && (w[i]&0xFF0000) && (w[i]&0xFF000000))++i;
            if(i>=in.count)return false;
            size_t start=result.size();
            result.insert(result.end(),w,w+i+1);
            for(++i;i<in.count;++i)if(!variables.count(w[i]))result.push_back(w[i]);
            result[start]=(uint32_t(result.size()-start)<<16)|spv::OpEntryPoint;
            continue;
        }
        size_t start=result.size();
        result.insert(result.end(),w,w+in.count);
        if(in.op==spv::OpVariable && variables.count(w[2])) {
            result[start+1]=variableType[w[2]];result[start+3]=spv::StorageClassPrivate;
        }
        auto insert=inserts.find(in.pos);
        if(insert!=inserts.end())result.insert(result.end(),insert->second.begin(),insert->second.end());
    }
    code.swap(result);
    return true;
}
}
