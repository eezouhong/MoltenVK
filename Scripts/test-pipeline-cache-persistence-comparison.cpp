// SPDX-License-Identifier: Apache-2.0
// Compiles the actual persistent metadata serializers and comparison body.
#include "SPIRVToMSLConverter.h"
#include <cereal/archives/binary.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define MVK_PUBLIC_SYMBOL
#define MVK_MACOS 1
#ifndef MVK_USE_CEREAL
#define MVK_USE_CEREAL 1
#endif

enum MVKConfigCompressionAlgorithm {
    MVK_CONFIG_COMPRESSION_ALGORITHM_NONE = 0,
    MVK_CONFIG_COMPRESSION_ALGORITHM_LZFSE = 1,
};

template <class C>
class MVKCompressor {
  public:
    std::vector<uint8_t> _compressed;
    size_t _uncompressedSize = 0;
    MVKConfigCompressionAlgorithm _algorithm =
        MVK_CONFIG_COMPRESSION_ALGORITHM_NONE;
};

// @PRODUCTION_CONFIG_METHODS@
// @PRODUCTION_METADATA_SERIALIZERS@

#ifdef INJECT_BAD_ALLOC
class TestOutputStream : public std::ostringstream {
  public:
    explicit TestOutputStream(std::ios::openmode mode)
        : std::ostringstream(mode) {
        throw std::bad_alloc();
    }
};
#define ostringstream TestOutputStream
#endif

#ifdef INJECT_CEREAL_EXCEPTION
namespace cereal {
class ThrowingBinaryOutputArchive {
  public:
    explicit ThrowingBinaryOutputArchive(std::ostream&) {}
    template <class... Values>
    void operator()(Values&&...) {
        throw cereal::Exception("injected archive failure");
    }
};
}
#define BinaryOutputArchive ThrowingBinaryOutputArchive
#endif

// @PRODUCTION_PERSISTENCE_COMPARATOR@

#ifdef INJECT_BAD_ALLOC
#undef ostringstream
#endif
#ifdef INJECT_CEREAL_EXCEPTION
#undef BinaryOutputArchive
#endif

using Config = mvk::SPIRVToMSLConversionConfiguration;
using ResultInfo = mvk::SPIRVToMSLConversionResultInfo;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static Config makeConfig(size_t resourceCount) {
    Config config;
    config.options.entryPointStage = spv::ExecutionModelFragment;
    for (size_t index = 0; index < resourceCount; ++index) {
        mvk::MSLResourceBinding resource;
        resource.resourceBinding.stage = spv::ExecutionModelFragment;
        resource.resourceBinding.desc_set = uint32_t(index % 4);
        resource.resourceBinding.binding = uint32_t(index);
        resource.resourceBinding.msl_buffer = uint32_t(index % 31);
        resource.outIsUsedByShader = (index % 3) != 0;
        config.resourceBindings.push_back(resource);
    }
    return config;
}

static MVKCompressor<std::string> makePayload(size_t compressedSize) {
    MVKCompressor<std::string> payload;
    payload._compressed.resize(compressedSize);
    for (size_t index = 0; index < compressedSize; ++index) {
        payload._compressed[index] = uint8_t(index * 17 + 3);
    }
    payload._uncompressedSize = compressedSize * 3;
    payload._algorithm = MVK_CONFIG_COMPRESSION_ALGORITHM_LZFSE;
    return payload;
}

int main() {
    try {
        Config config = makeConfig(256);
        ResultInfo result;
        result.entryPoint.mtlFunctionName = "main0";
        result.needsBufferSizeBuffer = true;
        MVKCompressor<std::string> payload = makePayload(256 * 1024);

#if defined(INJECT_BAD_ALLOC) || defined(INJECT_CEREAL_EXCEPTION) || !MVK_USE_CEREAL
        require(!mvkAreShaderLibraryPersistenceEqual(
                    config, result, payload, config, result, payload),
                "unavailable exact comparison was not conservative");
#else
        require(mvkAreShaderLibraryPersistenceEqual(
                    config, result, payload, config, result, payload),
                "identical persistent record differed");

        auto differentPayload = payload;
        differentPayload._compressed.back() ^= 1;
        require(!mvkAreShaderLibraryPersistenceEqual(
                    config, result, payload, config, result, differentPayload),
                "compressed bytes difference was ignored");
        differentPayload = payload;
        differentPayload._uncompressedSize++;
        require(!mvkAreShaderLibraryPersistenceEqual(
                    config, result, payload, config, result, differentPayload),
                "uncompressed size difference was ignored");
        differentPayload = payload;
        differentPayload._algorithm = MVK_CONFIG_COMPRESSION_ALGORITHM_NONE;
        require(!mvkAreShaderLibraryPersistenceEqual(
                    config, result, payload, config, result, differentPayload),
                "compression algorithm difference was ignored");

        auto differentResult = result;
        differentResult.needsDrawId = true;
        require(!mvkAreShaderLibraryPersistenceEqual(
                    config, result, payload, config, differentResult, payload),
                "result metadata difference was ignored");
        auto differentConfig = config;
        differentConfig.resourceBindings[1].outIsUsedByShader =
            !differentConfig.resourceBindings[1].outIsUsedByShader;
        require(!mvkAreShaderLibraryPersistenceEqual(
                    config, result, payload, differentConfig, result, payload),
                "config metadata difference was ignored");

        constexpr int Iterations = 100;
        auto started = std::chrono::steady_clock::now();
        for (int index = 0; index < Iterations; ++index) {
            require(mvkAreShaderLibraryPersistenceEqual(
                        config, result, payload, config, result, payload),
                    "timed identical comparison differed");
        }
        double elapsedMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        std::cout << "PERSISTENCE_COMPARE iterations=" << Iterations
                  << " resources=" << config.resourceBindings.size()
                  << " compressed_bytes=" << payload._compressed.size()
                  << " total_ms=" << elapsedMs
                  << " average_us=" << elapsedMs * 1000.0 / Iterations << '\n';
#endif
        std::cout << "PipelineCachePersistenceComparison PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PipelineCachePersistenceComparison FAIL: "
                  << error.what() << '\n';
        return 1;
    }
}
