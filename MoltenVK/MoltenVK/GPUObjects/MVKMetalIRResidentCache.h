#pragma once
#include <condition_variable>
#include <cstdint>
#include <list>
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mvkir {
// Device-owned recent artifacts. The byte budget measures serialized code, not
// Metal's private allocation footprint; an independent object limit is required.
// No Vulkan object is retained. Constructing callers share artifacts; completed
// pipelines retain independent immutable metadata rather than these Metal objects.
template<class Artifact> class ResidentCache {
public:
    struct Result {
        std::shared_ptr<Artifact> artifact;
        size_t bytes = 0;
        bool unsupported = false;
    };
    struct Stats {
        uint64_t hits = 0, misses = 0, waits = 0, evictions = 0;
        uint64_t metadataInspections = 0, metadataPrunes = 0;
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
                auto inserted = _entries.emplace(key, entry);
                try {
                    _metadata.push_back(key);
                } catch (...) {
                    _entries.erase(inserted.first);
                    throw;
                }
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
    using Metadata = std::list<std::string>;
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
        // Stable list cursors survive unordered-map rehashes. Bound work under
        // the global lock even when most artifacts are still owned by callers.
        const size_t budget = std::min<size_t>(_metadata.size(), 64);
        for (size_t inspected = 0; inspected < budget && _entries.size() >= _maxMetadata; ++inspected) {
            if (_prunePosition == _metadata.end()) _prunePosition = _metadata.begin();
            auto candidate = _prunePosition++;
            auto found = _entries.find(*candidate);
            ++_stats.metadataInspections;
            const auto& entry = found->second;
            // Preserve an in-use weak artifact as well as compiles, waiters and
            // resident objects. Removing its key would create a duplicate library.
            if (!entry->compiling && !entry->waiters && !entry->retained && entry->weak.expired()) {
                _entries.erase(found);
                _metadata.erase(candidate);
                ++_stats.metadataPrunes;
            }
        }
        // Live artifacts may exceed the soft metadata cap. Expired keys are
        // reclaimed incrementally as new requests arrive.
    }

    const size_t _maxObjects, _maxBytes, _maxMetadata;
    mutable std::mutex _lock;
    std::unordered_map<std::string, std::shared_ptr<Entry>> _entries;
    Metadata _metadata;
    typename Metadata::iterator _prunePosition = _metadata.end();
    Recent _recent;
    size_t _retainedBytes = 0;
    Stats _stats;
};
}
