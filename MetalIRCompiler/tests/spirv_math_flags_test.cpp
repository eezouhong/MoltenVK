#include "MVKSPIRVMathFlags.h"
#include "spirv_msl.hpp"
#include <cassert>
#include <fstream>
#include <cstdio>
int main(int argc,char** argv) {
    assert(argc>1);
    unsigned checked=0;
    for(int index=1;index<argc;++index) {
        std::ifstream file(argv[index],std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
        assert(bytes.size()>=20 && bytes.size()%4==0);
        std::vector<uint32_t> code(bytes.size()/4);memcpy(code.data(),bytes.data(),bytes.size());
        SPIRV_CROSS_NAMESPACE::CompilerMSL oracle(code);
        for(const auto& entry:oracle.get_entry_points_and_stages()) {
            oracle.set_entry_point(entry.name,entry.execution_model);
            auto actual=mvkshader::spirvMathFlags(code,entry.name.c_str(),entry.execution_model);
            assert(actual==oracle.get_fp_fast_math_flags(true));++checked;
        }
    }
    printf("SPIR-V math flags: %u entries match CompilerMSL\n",checked);
}
