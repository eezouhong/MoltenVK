#include "../../MoltenVK/MoltenVK/GPUObjects/MVKMetalIRFragmentOutputs.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>

// fragment_outputs_test IN OUT DEPTH STENCIL: drop the requested unattached outputs,
// write the module and, when it changed, check that no dropped builtin, its Output variable or its mode remains.
int main(int argc,char** argv) {
    assert(argc==5);
    std::ifstream file(argv[1],std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
    assert(bytes.size()>=20 && bytes.size()%4==0);
    std::vector<uint32_t> code(bytes.size()/4);memcpy(code.data(),bytes.data(),bytes.size());
    const auto original=code;
    bool depth=atoi(argv[3]), stencil=atoi(argv[4]);
    bool changed=mvkir::dropFragmentOutputs(code,depth,stencil);
    assert(changed==(code!=original));
    std::set<uint32_t> dropped;
    for(size_t pos=5;changed && pos<code.size();pos+=code[pos]>>16) {
        uint32_t op=code[pos]&65535;const uint32_t* w=code.data()+pos;
        if(op==spv::OpDecorate && w[2]==spv::DecorationBuiltIn) {
            assert(!(depth && w[3]==spv::BuiltInFragDepth));
            assert(!(stencil && w[3]==spv::BuiltInFragStencilRefEXT));
        }
        if(op==spv::OpExecutionMode) {
            assert(!(depth && (w[2]==spv::ExecutionModeDepthReplacing || w[2]==spv::ExecutionModeDepthGreater ||
                               w[2]==spv::ExecutionModeDepthLess || w[2]==spv::ExecutionModeDepthUnchanged)));
            assert(!(stencil && w[2]==spv::ExecutionModeStencilRefReplacingEXT));
        }
    }
    for(size_t pos=5;pos<original.size();pos+=original[pos]>>16) {
        const uint32_t* w=original.data()+pos;
        if((w[0]&65535)==spv::OpDecorate && w[2]==spv::DecorationBuiltIn &&
           ((depth && w[3]==spv::BuiltInFragDepth) || (stencil && w[3]==spv::BuiltInFragStencilRefEXT)))dropped.insert(w[1]);
    }
    for(size_t pos=5;changed && pos<code.size();pos+=code[pos]>>16)
        if((code[pos]&65535)==spv::OpVariable && dropped.count(code[pos+2]))assert(code[pos+3]==spv::StorageClassPrivate);
    if(changed) {
        auto again=code;
        assert(!mvkir::dropFragmentOutputs(again,depth,stencil) && again==code);
    } else assert(code==original);
    std::ofstream out(argv[2],std::ios::binary);out.write(reinterpret_cast<const char*>(code.data()),code.size()*4);
    printf("%s\n",changed?"changed":"unchanged");
}
