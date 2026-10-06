#pragma once
#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mvkir {
// Device-owned recent artifacts. The byte budget measures serialized code, not
// Metal's private allocation footprint; an independent object limit is required.
// No Vulkan object is retained. Active pipelines own artifacts independently.
template<class Artifact> class ResidentCache {
public:
    struct Result {
        std::shared_ptr<Artifact> artifact;
        size_t bytes = 0;
        bool unsupported = false;
    };
    struct Stats {
        uint64_t hits = 0, misses = 0, waits = 0, evictions = 0;
        size_t retained = 0, retainedBytes = 0, entries = 0;
    };
    ResidentCache(size_t objects, size_t bytes, size_t metadata = 4096)
        : _maxObjects(objects), _maxBytes(bytes), _maxMetadata(metadata) {}

    // Successful concurrent requests share one result even when retention is
    // disabled and the compiling caller immediately drops its reference.
    template<class Compile> std::shared_ptr<Artifact> get(const std::string& key, Compile compile) {
        std::shared_ptr<Entry> entry;
        std::vector<std::shared_ptr<Artifact>> retired;
        {
            std::unique_lock<std::mutex> lock(_lock);
            auto found = _entries.find(key);
            if (found == _entries.end()) {
                pruneMetadata();
                entry = std::make_shared<Entry>();
                _entries.emplace(key, entry);
            } else {
                entry = found->second;
            }
            while (entry->compiling) {
                ++entry->waiters;
                ++_stats.waits;
                entry->ready.wait(lock, [&] { return !entry->compiling; });
                auto shared = entry->handoff;
                if (--entry->waiters == 0) entry->handoff.reset();
                if (shared) {
                    ++_stats.hits;
                    return shared;
                }
            }
            if (auto artifact = entry->weak.lock()) {
                ++_stats.hits;
                retain(entry, artifact, retired);
                return artifact;
            }
            if (entry->unsupported) return {};
            entry->compiling = true;
            ++_stats.misses;
        }
        try {
            Result result = compile();
            {
                std::lock_guard<std::mutex> lock(_lock);
                entry->weak = result.artifact;
                entry->bytes = result.bytes;
                entry->unsupported = result.unsupported && !result.artifact;
                if (entry->waiters) entry->handoff = result.artifact;
                if (result.artifact) retain(entry, result.artifact, retired);
                entry->compiling = false;
                entry->ready.notify_all();
            }
            return result.artifact;
        } catch (...) {
            // Compiler or cache-allocation failure must never strand waiters.
            std::lock_guard<std::mutex> lock(_lock);
            entry->compiling = false;
            entry->ready.notify_all();
            throw;
        }
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(_lock);
        auto result = _stats;
        result.retained = _recent.size();
        result.retainedBytes = _retainedBytes;
        result.entries = _entries.size();
        return result;
    }

private:
    struct Entry;
    using Recent = std::list<std::shared_ptr<Entry>>;
    struct Entry {
        bool compiling = false, unsupported = false;
        size_t waiters = 0, bytes = 0;
        std::condition_variable ready;
        std::weak_ptr<Artifact> weak;
        std::shared_ptr<Artifact> retained, handoff;
        typename Recent::iterator position;
    };

    void retain(const std::shared_ptr<Entry>& entry, const std::shared_ptr<Artifact>& artifact,
                std::vector<std::shared_ptr<Artifact>>& retired) {
        if (entry->retained) {
            _recent.splice(_recent.begin(), _recent, entry->position);
            return;
        }
        if (!_maxObjects || !entry->bytes || entry->bytes > _maxBytes) return;
        // Reserve before changing ownership so allocation failure is harmless.
        retired.reserve(_recent.size());
        _recent.push_front(entry);
        entry->position = _recent.begin();
        entry->retained = artifact;
        _retainedBytes += entry->bytes;
        while (_recent.size() > _maxObjects || _retainedBytes > _maxBytes) {
            auto victim = _recent.back();
            _retainedBytes -= victim->bytes;
            retired.push_back(std::move(victim->retained));
            _recent.pop_back();
            ++_stats.evictions;
        }
        // retired destroys Metal objects only after the caller releases _lock.
    }

    void pruneMetadata() {
        if (_entries.size() < _maxMetadata) return;
        for (auto it = _entries.begin(); it != _entries.end() && _entries.size() >= _maxMetadata;) {
            const auto& entry = it->second;
            // Waiters retain their published result. Do not remove their key
            // and let a second compiler race the handoff.
            if (!entry->compiling && !entry->waiters && !entry->retained) it = _entries.erase(it);
            else ++it;
        }
        // Only live compiles and the bounded recent set may exceed metadata's
        // soft cap; idle weak/negative entries cannot grow without bound.
    }

    const size_t _maxObjects, _maxBytes, _maxMetadata;
    mutable std::mutex _lock;
    std::unordered_map<std::string, std::shared_ptr<Entry>> _entries;
    Recent _recent;
    size_t _retainedBytes = 0;
    Stats _stats;
};
}
