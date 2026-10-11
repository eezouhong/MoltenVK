// CPU-only structural oracle on a public synthetic MSC vertex artifact.
// GPU execution is tested separately by Tests/MetalIRReplay/run_discard.py.
#include "native_raster_adapter.h"
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

static uint64_t read64(const std::vector<uint8_t>& b,size_t offset) {
    if(offset>b.size()||b.size()-offset<8)throw std::runtime_error("truncated test artifact");
    uint64_t v;memcpy(&v,b.data()+offset,8);return v;
}
static std::unique_ptr<llvm::Module> parse(const std::vector<uint8_t>& b,llvm::LLVMContext& context) {
    auto start=read64(b,72),size=read64(b,80);
    if(start>b.size()||size>b.size()-start)throw std::runtime_error("invalid test artifact range");
    auto m=llvm::parseBitcodeFile(llvm::MemoryBufferRef(llvm::StringRef((const char*)b.data()+start,size),"discard test"),context);
    if(!m)throw std::runtime_error(llvm::toString(m.takeError()));return std::move(*m);
}
static llvm::Function* entry(llvm::Module& m) {
    auto* named=m.getNamedMetadata("air.vertex");
    if(!named||named->getNumOperands()!=1)throw std::runtime_error("vertex metadata missing");
    auto* md=llvm::dyn_cast<llvm::ConstantAsMetadata>(named->getOperand(0)->getOperand(0));
    auto* fn=md?llvm::dyn_cast<llvm::Function>(md->getValue()):nullptr;
    if(!fn)throw std::runtime_error("vertex function missing");return fn;
}
static std::vector<std::string> body(llvm::Function& f,unsigned& returns) {
    std::vector<std::string> result;
    for(auto& block:f)for(auto& inst:block) {
        if(llvm::isa<llvm::ReturnInst>(inst)) {++returns;continue;}
        std::string s;llvm::raw_string_ostream stream(s);inst.print(stream);result.push_back(stream.str());
    }
    return result;
}
int main(int argc,char** argv) {
    if(argc!=2)return 64;
    try {
        std::ifstream f(argv[1],std::ios::binary);
        std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)),{}),out;std::string error;
        if(b.size()<88||memcmp(b.data(),"MTLB",4)||read64(b,16)!=b.size())return 2;
        llvm::LLVMContext beforeContext,afterContext;
        auto before=parse(b,beforeContext);
        if(!melonx::air::discardVertexOutputs(b.data(),b.size(),out,error))throw std::runtime_error(error);
        auto after=parse(out,afterContext);auto* oldEntry=entry(*before);auto* newEntry=entry(*after);
        unsigned oldReturns=0,newReturns=0;
        if(llvm::verifyModule(*after,&llvm::errs())||!newEntry->getReturnType()->isVoidTy()||
           body(*oldEntry,oldReturns)!=body(*newEntry,newReturns)||!oldReturns||oldReturns!=newReturns)
            throw std::runtime_error("vertex body changed beyond returns");
        auto* stage=after->getNamedMetadata("air.vertex")->getOperand(0);
        auto* outputs=llvm::dyn_cast<llvm::MDNode>(stage->getOperand(1));
        if(!outputs||outputs->getNumOperands()||oldEntry->arg_size()!=newEntry->arg_size())return 1;
        for(auto& block:*newEntry)for(auto& inst:block)
            if(auto* ret=llvm::dyn_cast<llvm::ReturnInst>(&inst))if(ret->getReturnValue())return 1;
        for(size_t n:{size_t(0),size_t(87),b.size()-1})
            if(melonx::air::discardVertexOutputs(b.data(),n,out,error))return 1;
        auto invalid=b;uint64_t bad=UINT64_MAX;memcpy(invalid.data()+72,&bad,8);
        if(melonx::air::discardVertexOutputs(invalid.data(),invalid.size(),out,error))return 1;
        puts("PASS void vertex, unchanged non-return instructions, empty outputs and bounded malformed input rejection");
        return 0;
    } catch(const std::exception& e) {fprintf(stderr,"discard adapter test: %s\n",e.what());return 1;}
}
