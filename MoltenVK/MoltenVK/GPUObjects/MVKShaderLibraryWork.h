/* Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

/**
 * Short lookup -> module-local creation gate -> recheck -> build/publish.
 *
 * The gate key is a synchronization domain, NOT a compiled-result identity.
 * Lookup/publish must still compare the complete conversion configuration.
 * Serializing misses within one module avoids matching unconverted configs
 * using post-conversion 'used resource' masks. Hits bypass creation entirely,
 * and unrelated modules never wait for each other's compilation.
 *
 * Callbacks run synchronously. No background work or owner lifetime escapes
 * run(). As with Vulkan device objects, destruction requires callers to drain
 * their operations first. Failed builds/exceptions release the gate and allow
 * the next request to retry; no failed result is memoized here.
 */
template <class Key> class MVKShaderLibraryWork {
  public:
    template <class Lookup, class Build>
    auto run(const Key &key, bool allowCompile, Lookup lookup, Build build) -> decltype(lookup()) {
        if (auto result = lookup()) {
            return result;
        }
        if (!allowCompile) {
            return {};
        }

        auto gate = getGate(key);
        std::lock_guard<std::mutex> lock(*gate);
        if (auto result = lookup()) {
            return result;
        }
        return build();
    }

  private:
    std::shared_ptr<std::mutex> getGate(const Key &key) {
        std::lock_guard<std::mutex> lock(_lock);
        auto found = _gates.find(key);
        if (found != _gates.end()) {
            if (auto gate = found->second.lock()) {
                return gate;
            }
        }
        // Entries do not keep mutexes alive. Prune idle keys in bounded batches;
        // active owners AND waiters hold a shared_ptr, so cannot get split gates.
        if (_gates.size() >= 64) {
            for (auto it = _gates.begin(); it != _gates.end();) {
                if (it->second.expired()) {
                    it = _gates.erase(it);
                } else {
                    ++it;
                }
            }
        }
        auto gate = std::make_shared<std::mutex>();
        _gates[key] = gate;
        return gate;
    }

    std::mutex _lock;
    std::unordered_map<Key, std::weak_ptr<std::mutex>> _gates;
};
