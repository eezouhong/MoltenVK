#include "air_math_adapter.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/LLVMBitCodes.h>
#include <llvm/Bitstream/BitstreamReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/SHA256.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <vector>
using namespace llvm;
namespace melonx::air {
namespace {
template<class T> T take(Expected<T> result) {
    if (!result) throw std::runtime_error(toString(result.takeError()));
    return std::move(*result);
}
void check(Error error) { if (error) throw std::runtime_error(toString(std::move(error))); }
template<class T> T load(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size()-offset) throw std::runtime_error("range");
    T result; memcpy(&result,bytes.data()+offset,sizeof(T)); return result;
}
void bits(std::vector<uint8_t>& bytes,size_t bit,uint32_t value,unsigned count) {
    if(bit > bytes.size()*8 || count > bytes.size()*8-bit) throw std::runtime_error("bit range");
    for(unsigned i=0;i<count;++i) {
        auto mask=uint8_t(1u<<((bit+i)%8));
        bytes[(bit+i)/8]=(bytes[(bit+i)/8]&~mask)|(((value>>i)&1)?mask:0);
    }
}
struct Scanner {
    ArrayRef<uint8_t> raw;
    BitstreamCursor cursor;
    std::optional<BitstreamBlockInfo> blockInfo;
    std::vector<uint8_t>& output;
    size_t origin;
    bool change;
    unsigned marked=0;
    Scanner(ArrayRef<uint8_t> input,std::vector<uint8_t>& out,size_t base,bool adapt)
        :raw(input),cursor(input),output(out),origin(base),change(adapt) {}
    void block(unsigned id,bool root=false,unsigned depth=0) {
        if(depth>64) throw std::runtime_error("bitstream nesting limit");
        while(!cursor.AtEndOfStream()) {
            auto entry=take(cursor.advance());
            if(entry.Kind==BitstreamEntry::Error) throw std::runtime_error("invalid bitstream entry");
            if(entry.Kind==BitstreamEntry::EndBlock) {
                if(root) throw std::runtime_error("unexpected root block end");
                return;
            }
            if(entry.Kind==BitstreamEntry::SubBlock) {
                if(entry.ID==bitc::BLOCKINFO_BLOCK_ID) {
                    auto info=take(cursor.ReadBlockInfoBlock());
                    if(!info) throw std::runtime_error("blockinfo unavailable");
                    blockInfo=std::move(*info);cursor.setBlockInfo(&*blockInfo);
                } else {check(cursor.EnterSubBlock(entry.ID));block(entry.ID,false,depth+1);}
                continue;
            }
            size_t payload=cursor.GetCurrentBitNo();
            SmallVector<uint64_t,16> values;
            unsigned code=take(cursor.readRecord(entry.ID,values));
            if(id!=bitc::FUNCTION_BLOCK_ID || code!=bitc::FUNC_CODE_INST_BINOP ||
               values.size()!=4 || values.back()!=(bitc::NoSignedZeros|bitc::AllowReciprocal)) continue;
            if(entry.ID<bitc::FIRST_APPLICATION_ABBREV) throw std::runtime_error("unabbreviated marker");
            auto* abbreviation=take(cursor.getAbbrev(entry.ID));
            if(abbreviation->getNumOperandInfos()!=5 ||
               !abbreviation->getOperandInfo(0).isLiteral() ||
               abbreviation->getOperandInfo(0).getLiteralValue()!=code)
                throw std::runtime_error("unsupported marker abbreviation");
            SimpleBitstreamCursor measure(raw);check(measure.JumpToBit(payload));
            size_t flagBit=0;unsigned flagWidth=0;
            for(unsigned i=1;i<5;++i) {
                const auto& op=abbreviation->getOperandInfo(i);
                if(op.isLiteral()) throw std::runtime_error("literal marker operand");
                auto start=measure.GetCurrentBitNo();uint64_t value;
                if(op.getEncoding()==BitCodeAbbrevOp::VBR) value=take(measure.ReadVBR64(op.getEncodingData()));
                else if(op.getEncoding()==BitCodeAbbrevOp::Fixed) value=take(measure.Read(op.getEncodingData()));
                else throw std::runtime_error("unsupported marker encoding");
                if(value!=values[i-1]) throw std::runtime_error("marker decode mismatch");
                if(i==4) {
                    // Reassociation is bit 7, distinct from the legacy bit-0
                    // unsafe-algebra flag that also assumes away NaN/Inf.
                    if(op.getEncoding()!=BitCodeAbbrevOp::Fixed || op.getEncodingData()!=8)
                        throw std::runtime_error("marker width unsupported");
                    flagBit=start;flagWidth=op.getEncodingData();
                }
            }
            ++marked;
            if(change) bits(output,origin*8+flagBit,bitc::NoSignedZeros|bitc::AllowReciprocal|bitc::AllowReassoc|bitc::AllowContract,flagWidth);
        }
        if(!root) throw std::runtime_error("truncated block");
    }
};
} // namespace
bool relaxMathPermissions(const void* input,size_t size,std::vector<uint8_t>& output,
                          std::string& error,size_t* adjusted) {
    output.clear();error.clear();if(adjusted)*adjusted=0;
    try {
        if(!input || size<88 || size>64*1024*1024)throw std::runtime_error("invalid metallib size");
        const auto* begin=static_cast<const uint8_t*>(input);
        std::vector<uint8_t> bytes(begin,begin+size);
        if(memcmp(bytes.data(),"MTLB",4) || load<uint64_t>(bytes,16)!=size ||
           load<uint32_t>(bytes,load<uint64_t>(bytes,24))!=1)throw std::runtime_error("invalid metallib header");
        uint64_t offset=load<uint64_t>(bytes,72),length=load<uint64_t>(bytes,80);
        if(offset>size || length>size-offset || length<24 || load<uint32_t>(bytes,offset)!=0x0b17c0de ||
           load<uint32_t>(bytes,offset+4)!=0)throw std::runtime_error("invalid AIR wrapper");
        auto skip=load<uint32_t>(bytes,offset+8),extent=load<uint32_t>(bytes,offset+12);
        if(skip<20 || skip>length || extent>length-skip || extent<4)throw std::runtime_error("invalid AIR range");
        uint64_t cursor=load<uint64_t>(bytes,24)+8,limit=load<uint64_t>(bytes,40),hashOffset=0;
        if(cursor>limit || limit>offset)throw std::runtime_error("invalid function metadata range");
        while(cursor+6<=limit) {
            if(!memcmp(bytes.data()+cursor,"ENDT",4))break;
            auto fieldSize=load<uint16_t>(bytes,cursor+4);
            if(fieldSize>limit-cursor-6)throw std::runtime_error("invalid metadata size");
            if(!memcmp(bytes.data()+cursor,"HASH",4)) {
                if(fieldSize!=32||hashOffset)throw std::runtime_error("invalid AIR hash metadata");
                hashOffset=cursor+6;
            }
            cursor+=6+fieldSize;
        }
        auto beforeHash=SHA256::hash(ArrayRef<uint8_t>(bytes.data()+offset,length));
        if(!hashOffset || memcmp(bytes.data()+hashOffset,beforeHash.data(),32))
            throw std::runtime_error("AIR checksum mismatch");
        size_t base=offset+skip;
        Scanner scanner(ArrayRef<uint8_t>(bytes.data()+base,extent),bytes,base,true);
        if(take(scanner.cursor.Read(32))!=0xdec04342)throw std::runtime_error("invalid AIR bitcode magic");
        scanner.block(0,true);
        if(scanner.marked) {
            auto digest=SHA256::hash(ArrayRef<uint8_t>(bytes.data()+offset,length));
            memcpy(bytes.data()+hashOffset,digest.data(),32);
        }
        if(adjusted)*adjusted=scanner.marked;
        output=std::move(bytes);return true;
    } catch(const std::exception& failure) {error=failure.what();return false;}
}
} // namespace melonx::air
