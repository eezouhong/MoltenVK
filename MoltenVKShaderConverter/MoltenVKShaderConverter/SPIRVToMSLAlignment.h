/*
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace mvk {

// Hash only narrows the search. The existing full matches() predicate remains
// authoritative, including constexpr samplers and comparison-relevant padding.
// outIsUsedByShader is deliberately excluded from identity, just as before.
inline size_t combineShaderUsageHash(size_t seed, uint32_t value) {
    return seed ^ (std::hash<uint32_t>{}(value) + size_t(0x9e3779b9u) + (seed << 6) + (seed >> 2));
}

struct InterfaceUsageHash {
    template <class T> size_t operator()(const T *value) const {
        return combineShaderUsageHash(value->shaderVar.location, value->binding);
    }
};

struct ResourceUsageHash {
    template <class T> size_t operator()(const T *value) const {
        auto &binding = value->resourceBinding;
        size_t seed = combineShaderUsageHash(uint32_t(binding.stage), binding.desc_set);
        return combineShaderUsageHash(seed, binding.binding);
    }
};

template <class T> struct ShaderUsageEqual {
    bool operator()(const T *a, const T *b) const { return a->matches(*b); }
};

/** Align usage flags without repeatedly scanning a large resource layout.
 *
 * This is a call-local index, not a shader/pipeline cache and not a change to
 * conversion-configuration matching. No pointer survives this call. The last
 * fully matching source entry wins, exactly like the former nested loop; false
 * on the last duplicate must override an earlier true. Self-aliasing uses the
 * original loop: earlier writes can affect later reads of duplicate entries.
 */
template <class T, class Hash>
void alignShaderUsage(std::vector<T> &destination, const std::vector<T> &source, Hash hash) {
    // Small vectors are cheaper without allocation. Keep the same loop for
    // these cases, including no match, duplicates, and source/destination alias.
    if (&destination == &source || source.size() <= 16 || destination.size() <= 4) {
        for (auto &item : destination) {
            item.outIsUsedByShader = false;
            for (const auto &candidate : source) {
                if (item.matches(candidate)) {
                    item.outIsUsedByShader = candidate.outIsUsedByShader;
                }
            }
        }
        return;
    }

    std::unordered_map<const T *, const T *, Hash, ShaderUsageEqual<T>> lastMatch(0, hash);
    lastMatch.reserve(source.size());
    for (const auto &candidate : source) {
        lastMatch[&candidate] = &candidate;
    }
    for (auto &item : destination) {
        item.outIsUsedByShader = false;
        auto found = lastMatch.find(&item);
        if (found != lastMatch.end()) {
            item.outIsUsedByShader = found->second->outIsUsedByShader;
        }
    }
}

} // namespace mvk
