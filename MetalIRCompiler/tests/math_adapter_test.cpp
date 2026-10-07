#include "air_math_adapter.h"
#include <llvm/ADT/SmallVector.h>
#include <llvm/Bitcode/LLVMBitCodes.h>
#include <llvm/Bitstream/BitstreamWriter.h>
#include <llvm/Support/SHA256.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef NDEBUG
#error "This test requires active assertions"
#endif

namespace {
template<class T> void put(std::vector<uint8_t>& data, size_t offset, T value) {
    memcpy(data.data() + offset, &value, sizeof(value));
}
struct Fixture {
    std::vector<uint8_t> bytes;
    std::vector<size_t> flagBits;
};
// A structural AIR-format fixture, not an executable shader. The GPU suite
// separately checks real MSC-produced libraries and float32 results.
Fixture fixture(bool fixedFlags = true, bool markers = true, unsigned flagWidth = 8) {
    using namespace llvm;
    SmallVector<char, 0> payload;
    BitstreamWriter writer(payload);
    writer.Emit(0xdec04342, 32);
    writer.EnterSubblock(bitc::MODULE_BLOCK_ID, 3);
    SmallVector<uint64_t, 4> values{2};
    writer.EmitRecord(bitc::MODULE_CODE_VERSION, values);
    writer.EnterSubblock(bitc::TYPE_BLOCK_ID_NEW, 4);
    values = {3}; writer.EmitRecord(bitc::TYPE_CODE_NUMENTRY, values);
    values.clear(); writer.EmitRecord(bitc::TYPE_CODE_FLOAT, values);
    values = {32}; writer.EmitRecord(bitc::TYPE_CODE_INTEGER, values);
    // Legacy typed pointers must survive; parsing through LLVM IR would replace
    // this record with an opaque pointer in LLVM 17.
    values = {0, 1}; writer.EmitRecord(bitc::TYPE_CODE_POINTER, values);
    writer.ExitBlock();
    writer.EnterSubblock(bitc::FUNCTION_BLOCK_ID, 4);
    auto abbreviation = std::make_shared<BitCodeAbbrev>();
    abbreviation->Add(BitCodeAbbrevOp(bitc::FUNC_CODE_INST_BINOP));
    abbreviation->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));
    abbreviation->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::VBR, 6));
    abbreviation->Add(BitCodeAbbrevOp(BitCodeAbbrevOp::Fixed, 4));
    abbreviation->Add(BitCodeAbbrevOp(fixedFlags ? BitCodeAbbrevOp::Fixed : BitCodeAbbrevOp::VBR,
                                     fixedFlags ? flagWidth : 6));
    unsigned code = writer.EmitAbbrev(abbreviation);
    Fixture result;
    for (unsigned i = 0; i < 7; ++i) {
        values = {1, 1, bitc::BINOP_MUL, markers && i >= 2 ? 24u : 0u};
        writer.EmitRecord(bitc::FUNC_CODE_INST_BINOP, values, code);
        result.flagBits.push_back((160 + 20) * 8 + writer.GetCurrentBitNo() - (fixedFlags ? flagWidth : 6));
    }
    writer.ExitBlock(); writer.ExitBlock();
    result.bytes.resize(180 + payload.size(), 0);
    auto& bytes = result.bytes;
    memcpy(bytes.data(), "MTLB", 4);
    put<uint64_t>(bytes, 16, bytes.size());
    put<uint64_t>(bytes, 24, 88); put<uint64_t>(bytes, 40, 142);
    put<uint64_t>(bytes, 72, 160); put<uint64_t>(bytes, 80, 20 + payload.size());
    put<uint32_t>(bytes, 88, 1);
    memcpy(bytes.data() + 96, "HASH", 4); put<uint16_t>(bytes, 100, 32);
    memcpy(bytes.data() + 134, "ENDTENDT", 8);
    put<uint32_t>(bytes, 160, 0x0b17c0de); put<uint32_t>(bytes, 168, 20);
    put<uint32_t>(bytes, 172, payload.size()); put<uint32_t>(bytes, 176, UINT32_MAX);
    memcpy(bytes.data() + 180, payload.data(), payload.size());
    auto digest = SHA256::hash(ArrayRef<uint8_t>(bytes.data() + 160, bytes.size() - 160));
    memcpy(bytes.data() + 102, digest.data(), 32);
    return result;
}
}

int main(int argc, char** argv) {
    auto generated = fixture();
    std::vector<uint8_t> original = generated.bytes, adapted;
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(input), {});
    } else assert(argc == 1);
    std::string error;
    size_t changed = 0;
    assert(melonx::air::relaxMathPermissions(original.data(), original.size(), adapted, error, &changed));
    assert(changed == 5 && adapted.size() == original.size() && adapted != original);
    if (argc == 1) {
        // Only permitted bits and the checksum change, not types or metadata.
        auto expected = generated.bytes;
        for (unsigned i = 2; i < generated.flagBits.size(); ++i) {
            for (unsigned bit = 0; bit < 8; ++bit) {
                if ((llvm::bitc::AllowReassoc | llvm::bitc::AllowContract) & (1u << bit)) {
                    size_t pos = generated.flagBits[i] + bit;
                    expected[pos / 8] |= 1u << (pos % 8);
                }
            }
        }
        memcpy(expected.data() + 102, adapted.data() + 102, 32);
        assert(expected == adapted);
    }
    std::vector<uint8_t> repeated;
    assert(melonx::air::relaxMathPermissions(adapted.data(), adapted.size(), repeated, error, &changed));
    assert(changed == 0 && repeated == adapted);
    // A failure must never leave a partially modified library available.
    for (size_t size : {size_t(0), size_t(87), original.size() - 1}) {
        adapted = original;
        assert(!melonx::air::relaxMathPermissions(original.data(), size, adapted, error));
        assert(adapted.empty() && !error.empty());
    }
    auto corrupted = original;
    uint64_t offset;
    memcpy(&offset, corrupted.data() + 72, sizeof(offset));
    corrupted[offset] ^= 1;
    assert(!melonx::air::relaxMathPermissions(corrupted.data(), corrupted.size(), adapted, error));
    assert(adapted.empty());
    auto noMarkers = fixture(true, false);
    assert(melonx::air::relaxMathPermissions(noMarkers.bytes.data(), noMarkers.bytes.size(), adapted, error, &changed));
    assert(changed == 0 && adapted == noMarkers.bytes);
    auto unsupported = fixture(false);
    assert(!melonx::air::relaxMathPermissions(unsupported.bytes.data(), unsupported.bytes.size(), adapted, error));
    assert(adapted.empty() && !error.empty());
    for (unsigned width : {6u, 7u}) {
        auto narrow = fixture(true, true, width);
        assert(!melonx::air::relaxMathPermissions(narrow.bytes.data(), narrow.bytes.size(), adapted, error));
        assert(adapted.empty() && !error.empty());
    }
    puts("AIR permission adapter: typed-pointer preservation, precise/partial flags, idempotence, checksum, truncation, no-op and unsupported-encoding checks passed");
}
