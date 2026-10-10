// CPU-only regression: use unadapted MSC vertex artifacts with native VertexID.
// Run for the real iOS17 and macOS26 synthetic producer outputs. No GPU is used.
#include "native_raster_adapter.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

static uint64_t read64(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t value;
    memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

int main(int argc, char** argv) {
    if (argc != 2) return 64;
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.size() < 88 || memcmp(bytes.data(), "MTLB", 4) || read64(bytes, 16) != bytes.size()) return 2;
    uint64_t start = read64(bytes, 24), limit = read64(bytes, 40);
    if (start > limit || limit > bytes.size() || limit - start < 8) return 2;
    size_t cursor = size_t(start) + 8, versionOffset = 0;
    while (limit - cursor >= 6 && memcmp(bytes.data() + cursor, "ENDT", 4)) {
        uint16_t size;
        memcpy(&size, bytes.data() + cursor + 4, sizeof(size));
        if (size > limit - cursor - 6) return 2;
        if (!memcmp(bytes.data() + cursor, "VERS", 4)) {
            if (size != 8) return 2;
            versionOffset = cursor + 6;
            break;
        }
        cursor += 6 + size;
    }
    if (!versionOffset) return 2;
    std::vector<uint8_t> output;
    std::string error;
    if (!melonx::air::restoreNativeRasterIO(bytes.data(), bytes.size(), 0,
                                           melonx::air::VertexID, output, error)) {
        fprintf(stderr, "Supported producer artifact rejected: %s\n", error.c_str());
        return 1;
    }
    const std::array<std::array<uint16_t, 4>, 4> rejected = {{
        {2, 8, 3, 1}, {2, 6, 4, 0}, {2, 7, 3, 2}, {9, 9, 9, 9},
    }};
    for (const auto& tuple : rejected) {
        auto invalid = bytes;
        memcpy(invalid.data() + versionOffset, tuple.data(), 8);
        if (melonx::air::restoreNativeRasterIO(invalid.data(), invalid.size(), 0,
                                              melonx::air::VertexID, output, error) ||
            error.find("unsupported MSC AIR or language version") == std::string::npos) return 1;
    }
    puts("PASS supported MSC raster artifact and four invalid/mixed version pairs");
    return 0;
}
