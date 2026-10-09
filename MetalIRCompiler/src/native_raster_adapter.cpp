#include "native_raster_adapter.h"
#include "air_legacy_memory.h"
#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/Bitcode/LLVMBitCodes.h>
#include <llvm/Bitstream/BitstreamReader.h>
#include <llvm/Bitstream/BitstreamWriter.h>
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
#include <map>
#include <optional>
#include <stdexcept>

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

template<class T> T checked(llvm::Expected<T> value) {
    if(!value)throw std::runtime_error(llvm::toString(value.takeError()));return std::move(*value);
}
void checked(llvm::Error value) {if(value)throw std::runtime_error(llvm::toString(std::move(value)));}

// LLVM 17's IR reader upgrades typed pointers to opaque pointers. Serializing
// that Module loses the pointee types consumed by Apple's bounds instrumentation.
// Rewrite raw metadata records instead; keep all type/instruction record values.
class RasterBitstream {
    llvm::BitstreamCursor reader;
    llvm::BitstreamWriter writer;
    std::optional<llvm::BitstreamBlockInfo> blockInfo;
    std::map<std::string,uint64_t> strings;
    uint32_t execution,requiredIO;
    bool globalNamesInStringTable=false;
    unsigned stringBlocks=0;uint32_t changed=0;

    std::string rewriteStrings(llvm::SmallVectorImpl<uint64_t>& values,llvm::StringRef blob) {
        using namespace llvm;
        if(values.size()!=2||!values[0]||values[0]>65536||values[1]>blob.size()||++stringBlocks!=1)
            throw std::runtime_error("unsupported AIR metadata string table");
        SimpleBitstreamCursor lengths(ArrayRef<uint8_t>((const uint8_t*)blob.data(),values[1]));
        SmallVector<char,0> encoded;BitstreamWriter lengthWriter(encoded);
        std::string characters;size_t offset=values[1];
        const char* marker=execution==0?"user(melonx_native_point_size0)":"user(melonx_native_point_coord0)";
        for(uint64_t i=0;i<values[0];++i) {
            auto size=checked(lengths.ReadVBR(6));
            if(size>blob.size()-offset)throw std::runtime_error("truncated AIR metadata string");
            std::string value=blob.substr(offset,size).str();offset+=size;
            // Metadata node operands encode MDString slots as slot + 1.
            strings.emplace(value,i+1);
            if(value==marker)value=execution==0?"air.point_size":"air.point_coord";
            if((requiredIO&VertexID)&&value=="user(melonx_native_vertex_id0)")value="air.vertex_id";
            if((requiredIO&InstanceID)&&value=="user(melonx_native_instance_id0)")value="air.instance_id";
            lengthWriter.EmitVBR(value.size(),6);characters+=value;
        }
        lengthWriter.FlushToWord();values[1]=encoded.size();
        return std::string(encoded.begin(),encoded.end())+characters;
    }
    void rewriteNode(llvm::SmallVectorImpl<uint64_t>& values) {
        const char* markers[]={"user(melonx_native_point_size0)","user(melonx_native_point_coord0)","user(melonx_native_vertex_id0)","user(melonx_native_instance_id0)"};
        for(unsigned i=0;i<4;++i) {
            uint32_t bit=1u<<i;if(!(requiredIO&bit))continue;
            auto found=strings.find(markers[i]);if(found==strings.end())continue;
            if(std::find(values.begin(),values.end(),found->second)==values.end())continue;
            auto type=strings.at("air.arg_type_name"),name=strings.at("air.arg_name");
            auto typePos=std::find(values.begin(),values.end(),type),namePos=std::find(values.begin(),values.end(),name);
            if(typePos==values.end()||namePos==values.end()||std::next(typePos)==values.end()||std::next(namePos)==values.end())throw std::runtime_error("unsupported AIR native metadata node");
            uint64_t typeValue=*std::next(typePos),nameValue=*std::next(namePos),index=values.front();
            const char* expected=i==0?"float":i==1?"float2":"uint";
            if(typeValue!=strings.at(expected)||(changed&bit))throw std::runtime_error("unexpected AIR native type or duplicate node");
            values.clear();if(i)values.push_back(index);
            values.append({found->second,type,typeValue,name,nameValue});changed|=bit;return;
        }
    }
    void block(unsigned id,bool root=false,unsigned depth=0) {
        using namespace llvm;
        if(depth>64)throw std::runtime_error("AIR nesting limit");
        while(!reader.AtEndOfStream()) {
            auto entry=checked(reader.advance());
            if(entry.Kind==BitstreamEntry::Error)throw std::runtime_error("invalid AIR bitstream");
            if(entry.Kind==BitstreamEntry::EndBlock) {if(root)throw std::runtime_error("unexpected AIR root end");return;}
            if(entry.Kind==BitstreamEntry::SubBlock) {
                if(entry.ID==bitc::BLOCKINFO_BLOCK_ID) {
                    auto info=checked(reader.ReadBlockInfoBlock());if(!info)throw std::runtime_error("AIR blockinfo missing");
                    blockInfo=std::move(*info);reader.setBlockInfo(&*blockInfo);
                } else if(entry.ID==bitc::SYMTAB_BLOCK_ID) {
                    // Optional summary contains byte offsets into the old file.
                    checked(reader.SkipBlock());
                } else {
                    checked(reader.EnterSubBlock(entry.ID));writer.EnterSubblock(entry.ID,6);
                    block(entry.ID,false,depth+1);writer.ExitBlock();
                }
                continue;
            }
            bool hasBlob=false;
            if(entry.ID>=bitc::FIRST_APPLICATION_ABBREV) {
                auto* abbreviation=checked(reader.getAbbrev(entry.ID));
                for(unsigned i=0;i<abbreviation->getNumOperandInfos();++i) {
                    const auto& op=abbreviation->getOperandInfo(i);
                    hasBlob|=!op.isLiteral()&&op.getEncoding()==BitCodeAbbrevOp::Blob;
                }
            }
            SmallVector<uint64_t,16> values;StringRef blob;
            unsigned code=checked(reader.readRecord(entry.ID,values,&blob));
            if(id==bitc::MODULE_BLOCK_ID&&code==bitc::MODULE_CODE_VERSION) {
                if(values.size()!=1)throw std::runtime_error("invalid AIR module version");
                globalNamesInStringTable=values[0]>=2;
            }
            if(id==bitc::MODULE_BLOCK_ID&&code==bitc::MODULE_CODE_VSTOFFSET)continue;
            if(id==bitc::METADATA_BLOCK_ID&&(code==bitc::METADATA_INDEX_OFFSET||code==bitc::METADATA_INDEX))continue;
            std::string replacement;
            if(id==bitc::METADATA_BLOCK_ID&&code==bitc::METADATA_STRINGS) {replacement=rewriteStrings(values,blob);blob=replacement;}
            if(id==bitc::METADATA_BLOCK_ID&&(code==bitc::METADATA_NODE||code==bitc::METADATA_DISTINCT_NODE))rewriteNode(values);
            if(id==bitc::VALUE_SYMTAB_BLOCK_ID&&code==bitc::VST_CODE_FNENTRY) {
                if(globalNamesInStringTable&&values.size()==2&&!hasBlob)continue;
                if(values.size()<2||(!hasBlob&&values.size()<3))throw std::runtime_error("invalid AIR function name record");
                values.erase(values.begin()+1);code=bitc::VST_CODE_ENTRY;
            }
            if(!hasBlob)writer.EmitRecord(code,values);
            else {
                auto abbreviation=std::make_shared<BitCodeAbbrev>();abbreviation->Add(BitCodeAbbrevOp(code));
                for(size_t i=0;i<values.size();++i)abbreviation->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR,6));
                abbreviation->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Blob));
                unsigned number=writer.EmitAbbrev(std::move(abbreviation));values.insert(values.begin(),code);
                writer.EmitRecordWithBlob(number,values,blob);
            }
        }
        if(!root)throw std::runtime_error("truncated AIR block");
    }
public:
    RasterBitstream(llvm::ArrayRef<uint8_t> source,llvm::SmallVectorImpl<char>& output,uint32_t stage,uint32_t required)
        :reader(source),writer(output),execution(stage),requiredIO(required) {}
    void run() {
        if(checked(reader.Read(32))!=0xdec04342)throw std::runtime_error("invalid AIR magic");
        writer.Emit(0xdec04342,32);block(0,true);writer.FlushToWord();
        if(changed!=requiredIO)throw std::runtime_error("AIR native raster nodes missing or unexpected");
    }
};
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
    if(!function){error="AIR native function unavailable";return false;}
    uint32_t found=0;auto& context=module.getContext();
    const char* markers[]={"user(melonx_native_point_size0)","user(melonx_native_point_coord0)","user(melonx_native_vertex_id0)","user(melonx_native_instance_id0)"};
    const char* builtins[]={"air.point_size","air.point_coord","air.vertex_id","air.instance_id"};
    for(unsigned k=0;k<4;++k) {
        const uint32_t bit=1u<<k;if(!(requiredIO&bit))continue;
        auto* group=dyn_cast<MDNode>(stage->getOperand(k==0?1:2));
        if(!group){error="AIR native interface group missing";return false;}
        for(unsigned i=0;i<group->getNumOperands();++i) {
            auto* item=dyn_cast<MDNode>(group->getOperand(i));if(!item)continue;
            bool matches=false;for(const auto& operand:item->operands())if(auto* string=dyn_cast_or_null<MDString>(operand))matches|=string->getString()==markers[k];
            if(!matches)continue;
            if(found&bit){error="duplicate AIR native input";return false;}
            SmallVector<Metadata*,8> replacement;
            if(k==0) {
                auto* result=dyn_cast<StructType>(function->getReturnType());
                if(!result||i>=result->getNumElements()||!result->getElementType(i)->isFloatTy()){error="AIR point size is not scalar float";return false;}
            } else {
                auto* index=dyn_cast<ConstantAsMetadata>(item->getOperand(0));auto* integer=index?dyn_cast<ConstantInt>(index->getValue()):nullptr;
                uint64_t arg=integer?integer->getZExtValue():UINT64_MAX;
                if(arg>=function->arg_size()){error="AIR native argument missing";return false;}
                Type* type=function->getArg(arg)->getType();
                if(k==1){auto* vec=dyn_cast<FixedVectorType>(type);if(!vec||vec->getNumElements()!=2||!vec->getElementType()->isFloatTy()){error="AIR point coordinates not float2";return false;}}
                else if(!type->isIntegerTy(32)){error="AIR native ID not i32";return false;}
                replacement.push_back(index);
            }
            const char* typeName=k==0?"float":k==1?"float2":"uint";
            replacement.append({text(context,builtins[k]),text(context,"air.arg_type_name"),text(context,typeName),text(context,"air.arg_name"),text(context,builtins[k])});
            group->replaceOperandWith(i,MDNode::get(context,replacement));found|=bit;
        }
    }
    if(found!=requiredIO){
        error="AIR native interface missing or unexpected: required="+std::to_string(requiredIO)+", found="+std::to_string(found);
        return false;
    }
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
        (execution!=0 && execution!=4) || (requiredIO & ~(execution==0?(PointSize|VertexID|InstanceID):PointCoordinates)))
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
    // Read the original wrapper directly, not the pointer-upgraded LLVM Module.
    if(bitcodeSize<20)return fail("truncated AIR wrapper");
    uint32_t payloadOffset=read<uint32_t>(data,bitcodeOffset+8),payloadSize=read<uint32_t>(data,bitcodeOffset+12);
    if(read<uint32_t>(data,bitcodeOffset)!=0x0b17c0de||payloadOffset<20||payloadOffset>bitcodeSize||payloadSize>bitcodeSize-payloadOffset)
        return fail("unsupported AIR bitcode wrapper");
    try {
        RasterBitstream transform(ArrayRef<uint8_t>(data+bitcodeOffset+payloadOffset,payloadSize),serialized,execution,requiredIO);
        transform.run();
        LLVMContext validation;
        auto verified=parseBitcodeFile(MemoryBufferRef(StringRef(serialized.data(),serialized.size()),"retagged AIR"),validation);
        if(!verified){error=toString(verified.takeError());return false;}
    } catch(const std::exception& failure) {error=failure.what();return false;}
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
    // MSC's public attribute records are separate from AIR's interface nodes.
    // Retagged native IDs must also stop requesting vertex-fetch attributes.
    // Preserve all record extents, indices and ordinary attribute bytes.
    const uint32_t nativeIDs=requiredIO&(VertexID|InstanceID);
    if(execution==0 && nativeIDs) {
        uint64_t publicSize=read<uint64_t>(data,48);
        if(publicSize<8||publicOffset>size||publicSize>size-publicOffset||
           read<uint32_t>(data,publicOffset)!=publicSize)
            return fail("unsupported single-function public metadata");
        size_t field=publicOffset+4,end=publicOffset+publicSize;uint32_t attributes=0;
        while(field<end && !tag(data,size,field,"ENDT")) {
            if(end-field<6)return fail("truncated public attribute tag");
            uint16_t length=read<uint16_t>(data,field+4);
            if(length>end-field-6)return fail("public attribute tag exceeds record");
            if(tag(data,size,field,"VATT")) {
                if(length<2)return fail("truncated vertex attribute count");
                uint16_t count=read<uint16_t>(data,field+6);size_t entry=field+8,limit=field+6+length;
                if(count>32)return fail("unsupported vertex attribute count");
                for(unsigned i=0;i<count;++i) {
                    const auto* nul=static_cast<const uint8_t*>(memchr(data+entry,0,limit-entry));
                    if(!nul||limit-size_t(nul-data)-1<2)return fail("truncated vertex attribute name/index");
                    std::string name(reinterpret_cast<const char*>(data+entry),size_t(nul-(data+entry)));
                    size_t indexOffset=size_t(nul-data)+1;uint16_t index=read<uint16_t>(data,indexOffset);
                    uint32_t bit=name=="melonx_native_vertex_id0"?VertexID:name=="melonx_native_instance_id0"?InstanceID:0;
                    if(bit) {
                        if(!(nativeIDs&bit)||(attributes&bit)||!(index&0x8000))return fail("unexpected native vertex attribute record");
                        write<uint16_t>(output,indexOffset,index&0x7fff);attributes|=bit;
                    }
                    entry=indexOffset+2;
                }
                if(entry!=limit)return fail("unexpected vertex attribute trailing data");
            }
            field+=6+length;
        }
        if(field>end || end-field<4 || !tag(data,size,field,"ENDT"))
            return fail("public vertex metadata end tag missing");
        if(attributes!=nativeIDs)return fail("native vertex attribute metadata missing");
    }
    return true;
}
}
