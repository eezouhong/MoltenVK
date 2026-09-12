// SPDX-License-Identifier: Apache-2.0
// The runner inserts actual converter methods and the pinned original alignment body.
#include "SPIRVToMSLAlignment.h"
#include "SPIRVToMSLConverter.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <type_traits>
#define MVK_PUBLIC_SYMBOL
#define MVK_MACOS 1
static thread_local uint64_t comparisonCount = 0;
// @PRODUCTION_METHODS@
// @REFERENCE_ALIGNMENT@
using Config = mvk::SPIRVToMSLConversionConfiguration;
using AlignmentResource = mvk::MSLResourceBinding;
static void require(bool condition, const char *message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
template <class T> static bool identical(const std::vector<T> &a, const std::vector<T> &b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if constexpr (std::is_same_v<T, mvk::MSLResourceBinding>) {
            // Outer struct tail padding is not a field and need not survive C++
            // copies. Compare every actual field, including ignored sampler
            // bytes; retain the exact inner-byte comparisons used by matches().
            if (std::memcmp(&a[i].resourceBinding, &b[i].resourceBinding,
                            sizeof(a[i].resourceBinding)) ||
                std::memcmp(&a[i].constExprSampler, &b[i].constExprSampler,
                            sizeof(a[i].constExprSampler)) ||
                a[i].requiresConstExprSampler != b[i].requiresConstExprSampler ||
                a[i].outIsUsedByShader != b[i].outIsUsedByShader) {
                return false;
            }
        } else if constexpr (std::is_same_v<T, mvk::MSLShaderInterfaceVariable>) {
            if (std::memcmp(&a[i].shaderVar, &b[i].shaderVar, sizeof(a[i].shaderVar)) ||
                a[i].binding != b[i].binding || a[i].outIsUsedByShader != b[i].outIsUsedByShader) {
                return false;
            }
        } else if (std::memcmp(&a[i], &b[i], sizeof(T))) {
            return false;
        }
    }
    return true;
}
static void same(const Config &a, const Config &b) {
    require(identical(a.shaderInputs, b.shaderInputs) &&
                identical(a.shaderOutputs, b.shaderOutputs) &&
                identical(a.resourceBindings, b.resourceBindings),
            "alignment differs from original byte-for-byte");
    require(a.options.matches(b.options) &&
                identical(a.discreteDescriptorSets, b.discreteDescriptorSets) &&
                identical(a.dynamicBufferDescriptors, b.dynamicBufferDescriptors),
            "alignment changed non-usage state");
}
static Config makeConfig(std::mt19937 &rng, size_t count) {
    Config c;
    for (size_t i = 0; i < count; ++i) {
        AlignmentResource r;
        r.resourceBinding.stage = spv::ExecutionModel(rng() % 6);
        r.resourceBinding.desc_set = rng() % 3;
        r.resourceBinding.binding = rng() % 13;
        r.resourceBinding.msl_buffer = rng() % 5;
        r.requiresConstExprSampler = (rng() % 3) == 0;
        // A constexpr sampler is compared as bytes by the existing predicate.
        // Copy only a valid default object, then vary one object byte for identity tests.
        reinterpret_cast<unsigned char *>(&r.constExprSampler)[0] = rng() % 2;
        r.outIsUsedByShader = rng() % 2;
        if (i && rng() % 4 == 0) {
            r = c.resourceBindings[rng() % i];
            r.outIsUsedByShader = rng() % 2;
        }
        c.resourceBindings.push_back(r);
        if (i < 40) {
            mvk::MSLShaderInterfaceVariable v;
            v.shaderVar.location = rng() % 7;
            v.binding = rng() % 3;
            v.outIsUsedByShader = rng() % 2;
            c.shaderInputs.push_back(v);
            c.shaderOutputs.push_back(v);
        }
    }
    return c;
}
static void compare(Config destination, const Config &source) {
    auto expected = destination;
    auto sourceBefore = source;
    referenceAlign(expected, source);
    destination.alignWith(source);
    same(destination, expected);
    same(source, sourceBefore);
}
struct CollidingHash {
    size_t operator()(const AlignmentResource *) const { return 0; }
};
static void checkSemantics() {
    std::mt19937 rng(13807);
    const size_t sizes[] = {0, 1, 4, 5, 16, 17, 31, 64, 257};
    size_t tested = 0;
    for (auto n : sizes)
        for (auto m : sizes) {
            auto dst = makeConfig(rng, n), src = makeConfig(rng, m);
            compare(dst, src);
            ++tested;
        }
    for (size_t i = 0; i < 1000; ++i) {
        auto dst = makeConfig(rng, rng() % 100);
        auto src = dst;
        std::shuffle(src.resourceBindings.begin(), src.resourceBindings.end(), rng);
        for (auto &r : src.resourceBindings) {
            r.outIsUsedByShader = rng() % 2;
        }
        if (i % 3 == 0 && !src.resourceBindings.empty())
            src.resourceBindings.pop_back();
        if (i % 5 == 0 && !src.resourceBindings.empty())
            src.resourceBindings.push_back(src.resourceBindings.front());
        compare(dst, src);
        ++tested;
        auto a = dst, b = dst;
        referenceAlign(a, a);
        b.alignWith(b);
        same(a, b);
        ++tested;
        auto expected = dst;
        referenceAlign(expected, src);
        mvk::alignShaderUsage(dst.resourceBindings, src.resourceBindings, CollidingHash{});
        require(identical(dst.resourceBindings, expected.resourceBindings),
                "hash collision changed equality or duplicate priority");
        ++tested;
    }
    // Identical matching fields with conflicting flags: last FALSE wins; not OR,
    // first match, or same-position shortcuts. Exercise the indexed-size path.
    Config dst, src;
    AlignmentResource r;
    r.resourceBinding.binding = 37;
    r.outIsUsedByShader = true;
    dst.resourceBindings.assign(32, r);
    src.resourceBindings.assign(32, r);
    src.resourceBindings.back().outIsUsedByShader = false;
    compare(dst, src);
    dst.alignWith(src);
    for (auto &x : dst.resourceBindings)
        require(!x.outIsUsedByShader, "last false duplicate did not win");
    // Changing ignored constexpr bytes must not change matching when the flag is false.
    dst = src;
    for (auto &x : dst.resourceBindings)
        x.requiresConstExprSampler = false;
    src = dst;
    for (auto &x : src.resourceBindings)
        reinterpret_cast<unsigned char *>(&x.constExprSampler)[0] ^= 1;
    compare(dst, src);
    std::vector<std::future<void>> parallel;
    for (int t = 0; t < 4; ++t)
        parallel.push_back(std::async(std::launch::async, [t] {
            std::mt19937 r(700 + t);
            for (int j = 0; j < 100; ++j) {
                auto a = makeConfig(r, 80);
                compare(a, a);
            }
        }));
    for (auto &f : parallel)
        f.get();
    std::cout << "SEMANTICS pairs=" << tested + 2 + 400 << " mismatches=0 parallel_workers=4\n";
}
static void checkComparisonBound() {
#ifdef ALIGNMENT_COUNT_COMPARISONS
    Config dst;
    for (uint32_t i = 0; i < 1024; ++i) {
        AlignmentResource r;
        r.resourceBinding.stage = spv::ExecutionModelFragment;
        r.resourceBinding.binding = i;
        r.outIsUsedByShader = i % 2;
        dst.resourceBindings.push_back(r);
    }
    auto source = dst;
    auto expected = dst;
    comparisonCount = 0;
    referenceAlign(expected, source);
    auto oldCount = comparisonCount;
    comparisonCount = 0;
    dst.alignWith(source);
    auto newCount = comparisonCount;
    same(dst, expected);
    std::cout << "COMPARISON_BOUND original=" << oldCount << " actual=" << newCount
              << " limit=8192\n";
    require(newCount <= 8192, "large-layout alignment remains quadratic");
#endif
}
template <class T> static void readPod(std::ifstream &f, T &value) {
    static_assert(std::is_trivially_copyable<T>::value, "POD only");
    require(bool(f.read(reinterpret_cast<char *>(&value), sizeof(value))), "truncated capture");
}
template <class T> static void readVector(std::ifstream &f, std::vector<T> &v) {
    uint64_t n;
    readPod(f, n);
    require(n <= 65536, "oversized capture vector");
    v.resize(n);
    require(bool(f.read(reinterpret_cast<char *>(v.data()), n * sizeof(T))),
            "truncated capture vector");
}
static Config readConfig(std::ifstream &f) {
    Config c;
    readPod(f, c.options.mslOptions);
    uint64_t n;
    readPod(f, n);
    require(n < 4096, "oversized entry name");
    c.options.entryPointName.resize(n);
    require(bool(f.read(c.options.entryPointName.data(), n)), "truncated name");
    readPod(f, c.options.entryPointStage);
    readPod(f, c.options.tessPatchKind);
    readPod(f, c.options.numTessControlPoints);
    readPod(f, c.options.shouldFlipVertexY);
    readPod(f, c.options.shouldFixupClipSpace);
    readVector(f, c.shaderInputs);
    readVector(f, c.shaderOutputs);
    readVector(f, c.resourceBindings);
    readVector(f, c.discreteDescriptorSets);
    readVector(f, c.dynamicBufferDescriptors);
    return c;
}
static std::pair<Config, Config> readPair(const char *path) {
    std::ifstream f(path, std::ios::binary);
    require(bool(f), "missing capture");
    uint64_t magic;
    readPod(f, magic);
    require(magic == 0x4d564b414c494731ull, "invalid capture magic");
    std::array<uint64_t, 5> sizes;
    readPod(f, sizes);
    Config c;
    require(sizes == std::array<uint64_t, 5>{sizeof(c.options.mslOptions),
                                             sizeof(mvk::MSLShaderInterfaceVariable),
                                             sizeof(AlignmentResource),
                                             sizeof(mvk::DescriptorBinding), sizeof(bool)},
            "capture ABI mismatch");
    auto dst = readConfig(f), src = readConfig(f);
    require(f.peek() == std::ifstream::traits_type::eof(), "unexpected capture trailing bytes");
    return {dst, src};
}
static volatile uint64_t checksum = 0;
static double timed(std::vector<std::pair<Config, Config>> &pairs, bool original) {
    auto start = std::chrono::steady_clock::now();
    for (int repeat = 0; repeat < 4; ++repeat)
        for (auto &p : pairs) {
            if (original)
                referenceAlign(p.first, p.second);
            else
                p.first.alignWith(p.second);
            if (!p.first.resourceBindings.empty())
                checksum += p.first.resourceBindings.front().outIsUsedByShader;
        }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}
int main(int argc, char **argv) {
    try {
        checkSemantics();
        checkComparisonBound();
        std::vector<std::pair<Config, Config>> pairs;
        for (int i = 1; i < argc; ++i) {
            auto p = readPair(argv[i]);
            compare(p.first, p.second);
            std::cout << "CAPTURE " << i << " dst_resources=" << p.first.resourceBindings.size()
                      << " src_resources=" << p.second.resourceBindings.size()
                      << " dst_inputs=" << p.first.shaderInputs.size()
                      << " src_inputs=" << p.second.shaderInputs.size()
                      << " stage=" << p.first.options.entryPointStage << "\n";
            pairs.push_back(std::move(p));
        }
        std::cout << "CAPTURE_REPLAY pairs=" << pairs.size() << " mismatches=0\n";
#ifndef ALIGNMENT_COUNT_COMPARISONS
        if (!pairs.empty())
            for (int round = 0; round < 7; ++round) {
                double oldMs, newMs;
                if (round % 2) {
                    newMs = timed(pairs, false);
                    oldMs = timed(pairs, true);
                } else {
                    oldMs = timed(pairs, true);
                    newMs = timed(pairs, false);
                }
                std::cout << "BENCH round=" << round << " calls=" << pairs.size() * 4
                          << " original_ms=" << oldMs << " actual_ms=" << newMs << "\n";
            }
#endif
        std::cout << "PASS\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
