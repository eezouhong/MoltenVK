#include "native_raster_adapter.h"
#include "air_legacy_memory.h"
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>
#include <cstring>
#include <limits>

namespace melonx::air {
namespace {
template<class T> T read(const uint8_t* data,size_t offset) {
    T value;memcpy(&value,data+offset,sizeof(T));return value;
}
template<class T> void write(std::vector<uint8_t>& data,size_t offset,T value) {
    memcpy(data.data()+offset,&value,sizeof(T));
}
bool tag(const uint8_t* data,size_t size,size_t offset,const char* name) {
    return offset<=size && size-offset>=4 && !memcmp(data+offset,name,4);
}
llvm::MDString* text(llvm::LLVMContext& c,const char* value) {return llvm::MDString::get(c,value);}
bool compatibleMemory(llvm::AttributeList attributes) {
    for (unsigned index:attributes.indexes())
        for (llvm::Attribute attr:attributes.getAttributes(index))
            if (attr.isIntAttribute() && attr.getKindAsEnum()==llvm::Attribute::Memory &&
                !legacyMemory(attr.getMemoryEffects()).exact) return false;
    return true;
}
bool retag(llvm::Module& module,uint32_t execution,uint32_t requiredIO,std::string& error) {
    using namespace llvm;
    auto* named=module.getNamedMetadata(execution==0?"air.vertex":"air.fragment");
    if (!named || named->getNumOperands()!=1) {error="AIR stage metadata unavailable";return false;}
    MDNode* stage=named->getOperand(0);
    if (stage->getNumOperands()!=3) {error="AIR raster stage signature unsupported";return false;}
    auto* functionMD=dyn_cast<ConstantAsMetadata>(stage->getOperand(0));
    auto* function=functionMD?dyn_cast<Function>(functionMD->getValue()):nullptr;
    auto* group=dyn_cast<MDNode>(stage->getOperand(execution==0?1:2));
    if (!function || !group) {error="AIR raster interface unavailable";return false;}
    uint32_t found=0;auto& context=module.getContext();
    for (unsigned i=0;i<group->getNumOperands();++i) {
        auto* item=dyn_cast<MDNode>(group->getOperand(i));if (!item) continue;
        const char* marker=execution==0?"user(melonx_native_point_size0)":"user(melonx_native_point_coord0)";
        bool matches=false;
        for (const auto& operand:item->operands())
            if (auto* string=dyn_cast_or_null<MDString>(operand)) matches|=string->getString()==marker;
        if (!matches) continue;
        SmallVector<Metadata*,8> replacement;
        if (execution==0) {
            auto* result=dyn_cast<StructType>(function->getReturnType());
            if (!result || i>=result->getNumElements() || !result->getElementType(i)->isFloatTy()) {
                error="AIR point size is not a scalar float";return false;
            }
            replacement={text(context,"air.point_size"),text(context,"air.arg_type_name"),text(context,"float"),text(context,"air.arg_name"),text(context,"mtl_point_size")};
            found|=PointSize;
        } else {
            auto* index=dyn_cast<ConstantAsMetadata>(item->getOperand(0));
            auto* integer=index?dyn_cast<ConstantInt>(index->getValue()):nullptr;
            uint64_t argument=integer?integer->getZExtValue():UINT64_MAX;
            auto* type=argument<function->arg_size()?dyn_cast<FixedVectorType>(function->getArg(argument)->getType()):nullptr;
            if (!type || type->getNumElements()!=2 || !type->getElementType()->isFloatTy()) {
                error="AIR point coordinates are not float2";return false;
            }
            replacement={index,text(context,"air.point_coord"),text(context,"air.arg_type_name"),text(context,"float2"),text(context,"air.arg_name"),text(context,"mtl_point_coord")};
            found|=PointCoordinates;
        }
        group->replaceOperandWith(i,MDNode::get(context,replacement));
    }
    if (found!=requiredIO) {error="AIR native point interface missing or unexpected";return false;}
    for (Function& function:module) {
        if (!compatibleMemory(function.getAttributes())) {error="AIR memory effects cannot be encoded exactly";return false;}
        for (BasicBlock& block:function) for (Instruction& instruction:block)
            if (auto* call=dyn_cast<CallBase>(&instruction))
                if (!compatibleMemory(call->getAttributes())) {error="AIR call memory effects cannot be encoded exactly";return false;}
    }
    return true;
}
}

bool restoreNativeRasterIO(const void* bytes,size_t size,uint32_t execution,
                          uint32_t requiredIO,std::vector<uint8_t>& output,std::string& error) {
    using namespace llvm;
    output.clear();error.clear();
    auto fail=[&](const char* message){error=message;return false;};
    if (!bytes || size<88 || size>64*1024*1024 || !requiredIO ||
        (execution!=0 && execution!=4) || requiredIO!=(execution==0?PointSize:PointCoordinates))
        return fail("invalid native raster adapter input");
    auto* data=(const uint8_t*)bytes;
    if (!tag(data,size,0,"MTLB") || read<uint64_t>(data,16)!=size)
        return fail("invalid metallib header");
    uint64_t functionOffset=read<uint64_t>(data,24),publicOffset=read<uint64_t>(data,40);
    uint64_t bitcodeOffset=read<uint64_t>(data,72),bitcodeSize=read<uint64_t>(data,80);
    if (functionOffset<88 || functionOffset>size-8 || publicOffset>size ||
        bitcodeOffset<publicOffset || bitcodeOffset>size || bitcodeSize>size-bitcodeOffset ||
        !bitcodeSize || read<uint32_t>(data,functionOffset)!=1)
        return fail("unsupported metallib ranges or function count");
    LLVMContext context;
    auto module=parseBitcodeFile(MemoryBufferRef(StringRef((const char*)data+bitcodeOffset,bitcodeSize),"MetalIR raster adapter"),context);
    if (!module) {error=toString(module.takeError());return false;}
    if (!retag(**module,execution,requiredIO,error)) return false;
    SmallVector<char,0> serialized;
    raw_svector_ostream stream(serialized);
    WriteBitcodeToFile(**module,stream);
    std::vector<uint8_t> bitcode;
    if (serialized.size()<4) return fail("empty AIR serialization");
    if (read<uint32_t>((const uint8_t*)serialized.data(),0)==0x0b17c0de) {
        bitcode.assign(serialized.begin(),serialized.end());
    } else {
        bitcode.resize(20);
        write<uint32_t>(bitcode,0,0x0b17c0de);write<uint32_t>(bitcode,8,20);
        write<uint32_t>(bitcode,12,serialized.size());write<uint32_t>(bitcode,16,UINT32_MAX);
        bitcode.insert(bitcode.end(),serialized.begin(),serialized.end());
    }
    bitcode.resize((bitcode.size()+15)&~size_t(15),0);
    output.insert(output.end(),data,data+bitcodeOffset);
    output.insert(output.end(),bitcode.begin(),bitcode.end());
    output.insert(output.end(),data+bitcodeOffset+bitcodeSize,data+size);
    write<uint64_t>(output,16,output.size());write<uint64_t>(output,80,bitcode.size());
    auto digest=SHA256::hash(bitcode);
    size_t cursor=functionOffset+8;bool hashSeen=false,sizeSeen=false,versionSeen=false;
    while (!tag(data,size,cursor,"ENDT")) {
        if (cursor>publicOffset || publicOffset-cursor<6) return fail("invalid metallib function metadata");
        unsigned length=read<uint16_t>(data,cursor+4);
        if (length>publicOffset-cursor-6) return fail("invalid metallib function tag length");
        if (tag(data,size,cursor,"HASH")) {
            if (length!=32 || hashSeen) return fail("invalid metallib hash tag");
            memcpy(output.data()+cursor+6,digest.data(),32);hashSeen=true;
        } else if (tag(data,size,cursor,"MDSZ")) {
            if (length!=8 || sizeSeen) return fail("invalid metallib size tag");
            write<uint64_t>(output,cursor+6,bitcode.size());sizeSeen=true;
        } else if (tag(data,size,cursor,"VERS")) {
            if (length!=8 || read<uint16_t>(data,cursor+6)!=2 || read<uint16_t>(data,cursor+8)!=8 ||
                read<uint16_t>(data,cursor+10)!=4 || read<uint16_t>(data,cursor+12)!=0)
                return fail("unsupported MSC AIR or language version");
            versionSeen=true;
        }
        cursor+=6+length;
    }
    if (!hashSeen || !sizeSeen || !versionSeen) return fail("required metallib tags unavailable");
    cursor+=4;
    if (tag(data,size,cursor,"ENDT")) cursor+=4;
    while (cursor<publicOffset && !tag(data,size,cursor,"ENDT")) {
        if (publicOffset-cursor<6) return fail("invalid metallib extension");
        unsigned length=read<uint16_t>(data,cursor+4);
        if (length>publicOffset-cursor-6) return fail("invalid metallib extension length");
        if (tag(data,size,cursor,"HDYN") || tag(data,size,cursor,"RLST")) {
            if (length!=16) return fail("invalid metallib footer range");
            uint64_t start=read<uint64_t>(data,cursor+6),extent=read<uint64_t>(data,cursor+14);
            if (start<bitcodeOffset+bitcodeSize || start>size || extent>size-start)
                return fail("unsupported metallib footer range");
            write<uint64_t>(output,cursor+6,start-bitcodeSize+bitcode.size());
        }
        cursor+=6+length;
    }
    return true;
}
}
