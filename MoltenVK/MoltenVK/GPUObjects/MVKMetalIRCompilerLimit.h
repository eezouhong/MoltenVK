#pragma once
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace mvkir {
// Process-wide admission for the in-process frontends. Cache hits do not enter.
class CompilerLimit {
public:
    struct Stats { unsigned active=0, waiting=0, peak=0, limit=0; uint64_t batches=0, reliefs=0; };
    explicit CompilerLimit(unsigned limit) : _limit(std::max(1u, limit)) {}
    class Permit {
        CompilerLimit* owner;
    public:
        explicit Permit(CompilerLimit& value) : owner(&value) { owner->enter(); }
        Permit(const Permit&)=delete;
        Permit& operator=(const Permit&)=delete;
        ~Permit() { owner->leave(); }
    };
    template<class Relief> bool relieveWhenIdle(std::chrono::milliseconds quiet, Relief relief) {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (_active || _waiting || _maintenance || _epoch==_relievedEpoch ||
                Clock::now()-_lastActivity < quiet) return false;
            _maintenance=true;
            _relievedEpoch=_epoch;
        }
        try { relief(); }
        catch (...) { finishRelief(); throw; }
        finishRelief();
        return true;
    }
    Stats stats() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return {_active,_waiting,_peak,_limit,_epoch,_reliefs};
    }
private:
    using Clock=std::chrono::steady_clock;
    void enter() {
        std::unique_lock<std::mutex> lock(_mutex);
        ++_waiting;
        _ready.wait(lock,[&]{ return !_maintenance && _active<_limit; });
        --_waiting;
        ++_active;
        _peak=std::max(_peak,_active);
    }
    void leave() {
        std::lock_guard<std::mutex> lock(_mutex);
        --_active;
        if (!_active && !_waiting) ++_epoch;
        _lastActivity=Clock::now();
        _ready.notify_all();
    }
    void finishRelief() {
        std::lock_guard<std::mutex> lock(_mutex);
        _maintenance=false;
        ++_reliefs;
        _ready.notify_all();
    }
    const unsigned _limit;
    mutable std::mutex _mutex;
    std::condition_variable _ready;
    unsigned _active=0,_waiting=0,_peak=0;
    bool _maintenance=false;
    uint64_t _epoch=0,_relievedEpoch=0,_reliefs=0;
    Clock::time_point _lastActivity=Clock::now();
};
}
