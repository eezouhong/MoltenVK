#pragma once
#include "spirv.hpp"
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace mvkshader {
// Match CompilerMSL::get_fp_fast_math_flags(true) without constructing its IR.
// Legacy modules start with every permission; entry modes and operation
// decorations intersect them. FPFastMathDefault operands are constant IDs.
inline uint32_t spirvMathFlags(const std::vector<uint32_t>& code, const char* name,
                              spv::ExecutionModel model) {
    if(code.size()<5 || code[0]!=spv::MagicNumber)throw std::runtime_error("invalid SPIR-V header");
    uint32_t entry=0, flags=UINT32_MAX;
    std::unordered_map<uint32_t,uint32_t> constants, defaults;
    auto walk=[&](auto visit) {
        for(size_t pos=5;pos<code.size();) {
            uint32_t count=code[pos]>>16, op=code[pos]&65535;
            if(!count || count>code.size()-pos)throw std::runtime_error("malformed SPIR-V instruction");
            visit(op,count,code.data()+pos);pos+=count;
        }
    };
    walk([&](uint32_t op,uint32_t count,const uint32_t* words) {
        if(op==spv::OpEntryPoint && count>=4 && words[1]==uint32_t(model)) {
            const char* text=reinterpret_cast<const char*>(words+3);
            if(!memchr(text,0,(count-3)*4))throw std::runtime_error("unterminated entry name");
            if(!strcmp(text,name))entry=words[2];
        }
        if((op==spv::OpConstant || op==spv::OpSpecConstant) && count>=4)constants[words[2]]=words[3];
        if(op==spv::OpConstantNull && count==3)constants[words[2]]=0;
        if(op==spv::OpDecorate && count>=4 && words[2]==spv::DecorationFPFastMathMode)flags&=words[3];
    });
    if(!entry)throw std::runtime_error("SPIR-V entry missing");
    walk([&](uint32_t op,uint32_t count,const uint32_t* words) {
        if(count<3 || words[1]!=entry)return;
        if(op==spv::OpExecutionMode) {
            if(words[2]==spv::ExecutionModeSignedZeroInfNanPreserve)
                flags&=~(uint32_t(spv::FPFastMathModeNSZMask)|spv::FPFastMathModeNotInfMask|spv::FPFastMathModeNotNaNMask);
            if(words[2]==spv::ExecutionModeContractionOff)flags&=~uint32_t(spv::FPFastMathModeAllowContractMask);
        }
        if(op==spv::OpExecutionModeId && count>=5 && words[2]==spv::ExecutionModeFPFastMathDefault)
            defaults[words[3]]=words[4];
    });
    for(const auto& value:defaults)if(value.second) {
        auto found=constants.find(value.second);
        if(found==constants.end())throw std::runtime_error("FPFastMathDefault constant missing");
        flags&=found->second;
    }
    return flags;
}
}
