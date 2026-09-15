/* Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>

/**
 * Short lookup -> module-local creation gate -> recheck -> build/publish.
 * The gate key is a synchronization domain, NOT a compiled-result identity.
 * Lookup/publish still compare the complete conversion configuration. Missing
 * variants within one module serialize; ready hits and other modules do not.
 * No owner or background work escapes run(). Vulkan valid lifetimes apply.
 */
template <class Key> class MVKShaderLibraryWork {
  public:
    struct Timing {
        uint64_t calls = 0;
        uint64_t readyHits = 0;
        uint64_t noCompileMisses = 0;
        uint64_t recheckHits = 0;
        uint64_t buildCalls = 0;
        uint64_t buildFailures = 0;
        uint64_t exceptions = 0;
        uint64_t totalNs = 0;
        uint64_t lookupNs = 0;
        uint64_t gateNs = 0;
        uint64_t recheckNs = 0;
        uint64_t buildNs = 0;
        uint64_t maximumCallNs = 0;
    };

    // Configure before publishing this repository to callers. Disabled runs
    // perform no clock reads or atomic counter updates.
    void enableTiming(bool enabled) { _timingEnabled = enabled; }
    bool timingEnabled() const { return _timingEnabled; }

    Timing timing() const {
        Timing value;
        auto fields = timingFields(value);
        for (size_t i = 0; i < fields.size(); ++i) {
            *fields[i] = _totals[i].load(std::memory_order_relaxed);
        }
        value.maximumCallNs = _maximumCallNs.load(std::memory_order_relaxed);
        return value;
    }

    template <class Lookup, class Build>
    auto run(const Key &key, bool allowCompile, Lookup lookup, Build build) -> decltype(lookup()) {
        if (!_timingEnabled) {
            return runImpl(key, allowCompile, lookup, build, nullptr);
        }
        Timing observation;
        observation.calls = 1;
        auto started = Clock::now();
        try {
            auto result = runImpl(key, allowCompile, lookup, build, &observation);
            observation.totalNs = elapsed(started);
            record(observation);
            return result;
        } catch (...) {
            observation.exceptions = 1;
            observation.totalNs = elapsed(started);
            record(observation);
            throw;
        }
    }

  private:
    using Clock = std::chrono::steady_clock;
    static uint64_t elapsed(Clock::time_point start) {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    }
    struct Span {
        uint64_t *destination;
        Clock::time_point start;
        explicit Span(uint64_t *value)
            : destination(value), start(value ? Clock::now() : Clock::time_point{}) {}
        ~Span() {
            if (destination) {
                *destination += elapsed(start);
            }
        }
    };
    // A single explicit field list keeps aggregation independent of layout and
    // padding. These are per-call wall-time SUMS, not total process elapsed time.
    static std::array<uint64_t *, 12> timingFields(Timing &v) {
        return {&v.calls,      &v.readyHits,     &v.noCompileMisses, &v.recheckHits,
                &v.buildCalls, &v.buildFailures, &v.exceptions,      &v.totalNs,
                &v.lookupNs,   &v.gateNs,        &v.recheckNs,       &v.buildNs};
    }
    void record(Timing &observation) {
        auto fields = timingFields(observation);
        for (size_t i = 0; i < fields.size(); ++i) {
            _totals[i].fetch_add(*fields[i], std::memory_order_relaxed);
        }
        uint64_t previous = _maximumCallNs.load(std::memory_order_relaxed);
        while (previous < observation.totalNs &&
               !_maximumCallNs.compare_exchange_weak(previous, observation.totalNs,
                                                     std::memory_order_relaxed)) {
        }
    }

    template <class Lookup, class Build>
    auto runImpl(const Key &key, bool allowCompile, Lookup &lookup, Build &build,
                 Timing *timing) -> decltype(lookup()) {
        {
            Span span(timing ? &timing->lookupNs : nullptr);
            if (auto result = lookup()) {
                if (timing) {
                    ++timing->readyHits;
                }
                return result;
            }
        }
        if (!allowCompile) {
            if (timing) {
                ++timing->noCompileMisses;
            }
            return {};
        }
        std::shared_ptr<std::mutex> gate;
        std::unique_lock<std::mutex> lock;
        {
            // Includes the short gate-registry lookup and the module mutex wait.
            // It does NOT count time spent doing our own compilation.
            Span span(timing ? &timing->gateNs : nullptr);
            gate = getGate(key);
            lock = std::unique_lock<std::mutex>(*gate);
        }
        {
            Span span(timing ? &timing->recheckNs : nullptr);
            if (auto result = lookup()) {
                if (timing) {
                    ++timing->recheckHits;
                }
                return result;
            }
        }
        if (timing) {
            ++timing->buildCalls;
        }
        Span span(timing ? &timing->buildNs : nullptr);
        auto result = build();
        if (timing && !result) {
            ++timing->buildFailures;
        }
        return result;
    }

    std::shared_ptr<std::mutex> getGate(const Key &key) {
        std::lock_guard<std::mutex> lock(_lock);
        auto found = _gates.find(key);
        if (found != _gates.end()) {
            if (auto gate = found->second.lock()) {
                return gate;
            }
        }
        // Owners AND waiters hold shared_ptrs, so pruning cannot split a live
        // gate. Expired-key cleanup starts only after the small registry fills.
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

    bool _timingEnabled = false;
    std::array<std::atomic<uint64_t>, 12> _totals{};
    std::atomic<uint64_t> _maximumCallNs{0};
    std::mutex _lock;
    std::unordered_map<Key, std::weak_ptr<std::mutex>> _gates;
};
