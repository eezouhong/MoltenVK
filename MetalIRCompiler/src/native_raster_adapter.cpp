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
#include <llvm/IR/Verifier.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/raw_ostream.h>
#include <cstring>
#include <limits>
#include <map>
#include <functional>
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
    int functionOrdinal=-1;
    using RecordFilter=std::function<void(unsigned,unsigned,int,llvm::SmallVectorImpl<uint64_t>&)>;
    RecordFilter filter;

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
            if(entry.Kind==BitstreamEntry::EndBlock) {
                if(root)throw std::runtime_error("unexpected AIR root end");
                // A filter may append complete unabbreviated records. These
                // are used for new types, avoiding forbidden forward refs.
                if(filter) {
                    SmallVector<uint64_t,16> added;filter(id,0,functionOrdinal,added);
                    size_t cursor=0;
                    while(cursor<added.size()) {
                        if(added.size()-cursor<2||added[cursor+1]>added.size()-cursor-2)
                            throw std::runtime_error("invalid AIR appended record");
                        unsigned code=added[cursor++];size_t count=added[cursor++];
                        writer.EmitRecord(code,ArrayRef<uint64_t>(added.data()+cursor,count));cursor+=count;
                    }
                }
                return;
            }
            if(entry.Kind==BitstreamEntry::SubBlock) {
                if(entry.ID==bitc::BLOCKINFO_BLOCK_ID) {
                    auto info=checked(reader.ReadBlockInfoBlock());if(!info)throw std::runtime_error("AIR blockinfo missing");
                    blockInfo=std::move(*info);reader.setBlockInfo(&*blockInfo);
                } else if(entry.ID==bitc::SYMTAB_BLOCK_ID) {
                    // Optional summary contains byte offsets into the old file.
                    checked(reader.SkipBlock());
                } else {
                    if(entry.ID==bitc::FUNCTION_BLOCK_ID)++functionOrdinal;
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
            if(requiredIO && id==bitc::METADATA_BLOCK_ID&&code==bitc::METADATA_STRINGS) {replacement=rewriteStrings(values,blob);blob=replacement;}
            if(requiredIO && id==bitc::METADATA_BLOCK_ID&&(code==bitc::METADATA_NODE||code==bitc::METADATA_DISTINCT_NODE))rewriteNode(values);
            if(filter)filter(id,code,functionOrdinal,values);
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
    RasterBitstream(llvm::ArrayRef<uint8_t> source,llvm::SmallVectorImpl<char>& output,uint32_t stage,uint32_t required,RecordFilter records={})
        :reader(source),writer(output),execution(stage),requiredIO(required),filter(std::move(records)) {}
    void run() {
        if(checked(reader.Read(32))!=0xdec04342)throw std::runtime_error("invalid AIR magic");
        writer.Emit(0xdec04342,32);block(0,true);writer.FlushToWord();
        if(changed!=requiredIO)throw std::runtime_error("AIR native raster nodes missing or unexpected");
    }
};

// Preserve MSC's typed pointers and every instruction/value ID. Return has no
// result ID, so dropping its operand cannot renumber subsequent instructions.
// Restrict the edit to the uniquely named, uncalled AIR vertex entry.
struct DiscardPlan {
    struct Record { unsigned code; std::vector<uint64_t> values; };
    std::vector<Record> types,metadata;
    struct FunctionRecord { uint64_t value,type; int definition; };
    std::vector<FunctionRecord> functions;
    uint64_t globals=0; unsigned version=0; int definitions=0;
    std::string metadataName;
    uint64_t stage=UINT64_MAX,output=UINT64_MAX,functionType=UINT64_MAX,voidType=UINT64_MAX;
    uint64_t functionValue=UINT64_MAX,functionMetadata=UINT64_MAX,functionRecordType=UINT64_MAX;
    uint64_t metadataPointerType=UINT64_MAX;
    int definition=-1;

    void scan(unsigned block,unsigned code,int,llvm::SmallVectorImpl<uint64_t>& values) {
        using namespace llvm;
        if(!code)return;
        if(block==bitc::TYPE_BLOCK_ID_NEW) {
            if(code==bitc::TYPE_CODE_NUMENTRY||code==bitc::TYPE_CODE_STRUCT_NAME)return;
            if(code==bitc::TYPE_CODE_VOID)voidType=types.size();
            types.push_back({code,{values.begin(),values.end()}});
        }
        if(block==bitc::MODULE_BLOCK_ID) {
            if(code==bitc::MODULE_CODE_VERSION) {
                if(values.size()!=1)throw std::runtime_error("invalid AIR module version");
                version=values[0];
            }
            if(code==bitc::MODULE_CODE_FUNCTION) {
                unsigned prefix=version>=2?2:0;
                if(values.size()<prefix+3)throw std::runtime_error("truncated AIR function record");
                bool prototype=values[prefix+2];
                functions.push_back({globals++,values[prefix],prototype?-1:definitions++});
            } else if(code==bitc::MODULE_CODE_GLOBALVAR||code==bitc::MODULE_CODE_ALIAS||
                      code==bitc::MODULE_CODE_ALIAS_OLD||code==bitc::MODULE_CODE_IFUNC)++globals;
        }
        if(block!=bitc::METADATA_BLOCK_ID)return;
        if(code==bitc::METADATA_NAME) {metadataName.assign(values.begin(),values.end());return;}
        if(code==bitc::METADATA_NAMED_NODE) {
            if(metadataName=="air.vertex") {
                if(values.size()!=1||stage!=UINT64_MAX)throw std::runtime_error("ambiguous AIR vertex entry");
                stage=values[0];
            }
            metadataName.clear();return;
        }
        if(code==bitc::METADATA_STRINGS) {
            if(values.size()!=2||values[0]>65536)throw std::runtime_error("invalid AIR metadata strings");
            metadata.resize(metadata.size()+values[0]);return;
        }
        if(code==bitc::METADATA_KIND||code==bitc::METADATA_ATTACHMENT||
           code==bitc::METADATA_GLOBAL_DECL_ATTACHMENT)return;
        if(code!=bitc::METADATA_VALUE&&code!=bitc::METADATA_NODE&&
           code!=bitc::METADATA_DISTINCT_NODE&&code!=bitc::METADATA_STRING_OLD)
            throw std::runtime_error("unsupported AIR discard metadata record "+std::to_string(code));
        metadata.push_back({code,{values.begin(),values.end()}});
    }
    void finish() {
        using namespace llvm;
        auto node=[&](uint64_t id)->const Record& {
            if(id>=metadata.size())throw std::runtime_error("invalid AIR discard metadata reference");
            return metadata[id];
        };
        const auto& entry=node(stage);
        if(entry.code!=bitc::METADATA_NODE||entry.values.size()!=3||!entry.values[0]||!entry.values[1])
            throw std::runtime_error("unsupported AIR discard entry metadata");
        output=entry.values[1]-1;functionMetadata=entry.values[0]-1;
        const auto& fn=node(functionMetadata);
        if(fn.code!=bitc::METADATA_VALUE||fn.values.size()!=2||node(output).code!=bitc::METADATA_NODE)
            throw std::runtime_error("unsupported AIR discard function or outputs");
        functionValue=fn.values[1];metadataPointerType=fn.values[0];
        for(const auto& f:functions)if(f.value==functionValue) {functionType=f.type;definition=f.definition;}
        functionRecordType=functionType;
        if(functionType>=types.size()||definition<0||voidType==UINT64_MAX)
            throw std::runtime_error("AIR discard function definition/void type missing");
        if(types[functionType].code==bitc::TYPE_CODE_POINTER)functionType=types[functionType].values.at(0);
        if(functionType>=types.size()||types[functionType].code!=bitc::TYPE_CODE_FUNCTION)
            throw std::runtime_error("unsupported AIR discard function type");
        if(metadataPointerType>=types.size()||types[metadataPointerType].code!=bitc::TYPE_CODE_POINTER||
           types[metadataPointerType].values.size()!=2||types[metadataPointerType].values[0]!=functionType)
            throw std::runtime_error("unsupported AIR discard metadata pointer type");
        unsigned users=0;
        for(const auto& f:functions) {
            auto t=f.type;if(t>=types.size())throw std::runtime_error("invalid AIR function type");
            if(types[t].code==bitc::TYPE_CODE_POINTER)t=types[t].values.at(0);
            users+=t==functionType;
        }
        if(users!=1)throw std::runtime_error("shared AIR discard function type");
        // Mutating a shared/uniqued node would change another interface too.
        unsigned references=0;
        for(const auto& m:metadata)if(m.code==bitc::METADATA_NODE||m.code==bitc::METADATA_DISTINCT_NODE)
            references+=std::count(m.values.begin(),m.values.end(),output+1);
        // An already-empty output group may be uniqued with other empty nodes;
        // clearing it again changes nothing. Nonempty shared groups are unsafe.
        if(references!=1&&!node(output).values.empty())throw std::runtime_error("shared AIR raster output metadata");
    }
    unsigned nextMetadata=0,returns=0;uint64_t nextGlobal=0;
    void rewrite(unsigned block,unsigned code,int ordinal,llvm::SmallVectorImpl<uint64_t>& values) {
        using namespace llvm;
        if(block==bitc::TYPE_BLOCK_ID_NEW) {
            if(code==bitc::TYPE_CODE_NUMENTRY)values[0]+=2;
            if(code==0) {
                auto fn=types[functionType].values;fn.at(1)=voidType;
                values.append({bitc::TYPE_CODE_FUNCTION,fn.size()});values.append(fn.begin(),fn.end());
                auto ptr=types[metadataPointerType].values;ptr[0]=types.size();
                values.append({bitc::TYPE_CODE_POINTER,ptr.size()});values.append(ptr.begin(),ptr.end());
            }
        }
        if(block==bitc::MODULE_BLOCK_ID) {
            if(code==bitc::MODULE_CODE_FUNCTION) {
                if(nextGlobal++==functionValue)values[version>=2?2:0]=
                    types.size()+(types[functionRecordType].code==bitc::TYPE_CODE_POINTER?1:0);
            } else if(code==bitc::MODULE_CODE_GLOBALVAR||code==bitc::MODULE_CODE_ALIAS||
                      code==bitc::MODULE_CODE_ALIAS_OLD||code==bitc::MODULE_CODE_IFUNC)++nextGlobal;
        }
        if(block==bitc::METADATA_BLOCK_ID) {
            if(code==bitc::METADATA_STRINGS)nextMetadata+=values[0];
            else if(code==bitc::METADATA_VALUE||code==bitc::METADATA_NODE||
                    code==bitc::METADATA_DISTINCT_NODE||code==bitc::METADATA_STRING_OLD) {
                if(nextMetadata==functionMetadata)values[0]=types.size()+1;
                if(nextMetadata++==output)values.clear();
            }
        }
        if(block==bitc::FUNCTION_BLOCK_ID&&ordinal==definition&&code==bitc::FUNC_CODE_INST_RET) {
            values.clear();++returns;
        }
    }
};

llvm::Function* discardEntry(llvm::Module& module,bool transformed,std::string& error) {
    using namespace llvm;
    auto* named=module.getNamedMetadata("air.vertex");
    if(!named||named->getNumOperands()!=1) {error="AIR discard vertex entry unavailable";return nullptr;}
    auto* stage=named->getOperand(0);
    if(stage->getNumOperands()!=3) {error="AIR discard interface unsupported";return nullptr;}
    auto* md=dyn_cast<ConstantAsMetadata>(stage->getOperand(0));
    auto* function=md?dyn_cast<Function>(md->getValue()):nullptr;
    auto* outputs=dyn_cast<MDNode>(stage->getOperand(1));
    if(!function||function->isDeclaration()||!function->use_empty()||!outputs||
       (transformed&&(!function->getReturnType()->isVoidTy()||outputs->getNumOperands()))) {
        error="AIR discard entry/outputs invalid or entry referenced";return nullptr;
    }
    return function;
}
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

static bool adaptRaster(const void* bytes,size_t size,uint32_t execution,
                        uint32_t requiredIO,bool discard,std::vector<uint8_t>& output,std::string& error) {
    using namespace llvm;
    output.clear();error.clear();
    auto fail=[&](const char* message){error=message;return false;};
    if (!bytes || size<88 || size>64*1024*1024 || (!requiredIO&&!discard) ||
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
    if(discard) {if(!discardEntry(**module,false,error))return false;}
    else if (!retag(**module,execution,requiredIO,error)) return false;
    SmallVector<char,0> serialized;
    // Read the original wrapper directly, not the pointer-upgraded LLVM Module.
    if(bitcodeSize<20)return fail("truncated AIR wrapper");
    uint32_t payloadOffset=read<uint32_t>(data,bitcodeOffset+8),payloadSize=read<uint32_t>(data,bitcodeOffset+12);
    if(read<uint32_t>(data,bitcodeOffset)!=0x0b17c0de||payloadOffset<20||payloadOffset>bitcodeSize||payloadSize>bitcodeSize-payloadOffset)
        return fail("unsupported AIR bitcode wrapper");
    try {
        auto source=ArrayRef<uint8_t>(data+bitcodeOffset+payloadOffset,payloadSize);
        DiscardPlan plan;
        if(discard) {
            SmallVector<char,0> ignored;
            RasterBitstream scan(source,ignored,execution,0,[&](unsigned b,unsigned c,int f,SmallVectorImpl<uint64_t>& v){plan.scan(b,c,f,v);});
            scan.run();plan.finish();
        }
        RasterBitstream transform(source,serialized,execution,requiredIO,
            discard?std::function<void(unsigned,unsigned,int,SmallVectorImpl<uint64_t>&)>([&](unsigned b,unsigned c,int f,SmallVectorImpl<uint64_t>& v){plan.rewrite(b,c,f,v);}):nullptr);
        transform.run();
        LLVMContext validation;
        auto verified=parseBitcodeFile(MemoryBufferRef(StringRef(serialized.data(),serialized.size()),"retagged AIR"),validation);
        if(!verified){error=toString(verified.takeError());return false;}
        if(discard) {
            if(!plan.returns||!discardEntry(**verified,true,error))return false;
            std::string diagnostic;raw_string_ostream stream(diagnostic);
            if(verifyModule(**verified,&stream)) {error="invalid discarded AIR: "+stream.str();return false;}
        }
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
            if (length!=8) return fail("unsupported MSC version tag length");
            uint16_t airMajor=read<uint16_t>(data,cursor+6), airMinor=read<uint16_t>(data,cursor+8);
            uint16_t languageMajor=read<uint16_t>(data,cursor+10), languageMinor=read<uint16_t>(data,cursor+12);
            // MSC 3.1.1 emits AIR 2.6 / MSL 3.1 for the iOS 17 target,
            // and AIR 2.8 / MSL 4.0 for the macOS 26 target. Retag only these
            // verified pairs; the bitstream/interface checks remain strict.
            bool iosVersion=airMajor==2 && airMinor==6 && languageMajor==3 && languageMinor==1;
            bool macVersion=airMajor==2 && airMinor==8 && languageMajor==4 && languageMinor==0;
            if (!iosVersion && !macVersion) {
                error="unsupported MSC AIR or language version: AIR "+std::to_string(airMajor)+"."+std::to_string(airMinor)+
                    ", language "+std::to_string(languageMajor)+"."+std::to_string(languageMinor);
                return false;
            }
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
                        if(!(nativeIDs&bit)||(attributes&bit))return fail("unexpected native vertex attribute record");
                        // MSC already leaves the fetch flag clear when the
                        // shader declares the ID but never uses its value.
                        if(index&0x8000)write<uint16_t>(output,indexOffset,index&0x7fff);
                        attributes|=bit;
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
bool restoreNativeRasterIO(const void* bytes,size_t size,uint32_t execution,uint32_t requiredIO,
                          std::vector<uint8_t>& output,std::string& error) {
    return adaptRaster(bytes,size,execution,requiredIO,false,output,error);
}
bool discardVertexOutputs(const void* bytes,size_t size,std::vector<uint8_t>& output,std::string& error) {
    return adaptRaster(bytes,size,0,0,true,output,error);
}
}
