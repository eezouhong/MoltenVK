// Read-only inspection of real MSC AIR; never creates a Metal device.
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/FMF.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Error.h>
#include <fstream>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <cstdio>
template<class T>T read(const std::vector<char>& b,size_t p){if(p>b.size()||sizeof(T)>b.size()-p)throw std::runtime_error("extent");T v;memcpy(&v,b.data()+p,sizeof(v));return v;}
int main(int argc,char**argv){try{
 if(argc!=2)return 64;std::ifstream f(argv[1],std::ios::binary);std::vector<char>b((std::istreambuf_iterator<char>(f)),{});
 if(b.size()<88||memcmp(b.data(),"MTLB",4))throw std::runtime_error("not a metallib");
 size_t off=read<uint64_t>(b,72),len=read<uint64_t>(b,80);if(off>b.size()||len>b.size()-off||len<20)throw std::runtime_error("wrapper extent");
 if(read<uint32_t>(b,off)!=0x0b17c0de)throw std::runtime_error("wrapper magic");
 size_t inner=read<uint32_t>(b,off+8),bytes=read<uint32_t>(b,off+12);if(inner>len||bytes>len-inner)throw std::runtime_error("bitcode extent");
 llvm::LLVMContext ctx;auto mem=llvm::MemoryBuffer::getMemBuffer(llvm::StringRef(b.data()+off+inner,bytes),"air",false);
 auto parsed=llvm::parseBitcodeFile(mem->getMemBufferRef(),ctx);if(!parsed)throw std::runtime_error(llvm::toString(parsed.takeError()));
 auto& module=**parsed;unsigned f16=0,f32=0,flagged=0;
 for(auto& fn:module)for(auto& block:fn)for(auto& inst:block)if(auto* call=llvm::dyn_cast<llvm::CallBase>(&inst))if(auto* callee=call->getCalledFunction()){
  auto name=callee->getName();if(name=="air.fma.f16"||name=="air.fma.f32"){f16+=name=="air.fma.f16";f32+=name=="air.fma.f32";flagged+=call->getFastMathFlags().any();call->print(llvm::errs());llvm::errs()<<"\n";}
 }
 printf("{\"airFma16Calls\":%u,\"airFma32Calls\":%u,\"flaggedFmaCalls\":%u}\n",f16,f32,flagged);
 // Flags are diagnostic output, not an independent semantic verdict. Compare
 // their permissions with the source operation and numerical oracle.
 return f16+f32>0?0:1;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 2;}}
