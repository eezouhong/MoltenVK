// Diagnostic-only frame assembly. The caller serializes access; no GPU waits.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace mvkreplay {
struct GPUInterval { uint64_t begin, end; };
inline uint64_t gpuIntervalUnion(std::vector<GPUInterval> intervals) {
    if (intervals.empty()) return 0;
    std::sort(intervals.begin(), intervals.end(), [](auto a, auto b) { return a.begin < b.begin; });
    uint64_t total = 0, begin = intervals[0].begin, end = intervals[0].end;
    for (size_t i = 1; i < intervals.size(); ++i) {
        if (intervals[i].begin > end) { total += end - begin; begin = intervals[i].begin; end = intervals[i].end; }
        else end = std::max(end, intervals[i].end);
    }
    return total + end - begin;
}
struct FrameValues {
    uint64_t sealedNs = 0, encodeNs = 0, descriptorNs = 0, shadowNs = 0;
    uint64_t renderEncoders = 0, blitEncoders = 0, computeEncoders = 0;
    uint64_t passBreaks = 0, irPassBreaks = 0, indirectDraws = 0, indirectDispatches = 0;
    uint64_t parameterBytes = 0, directUploads = 0;
    uint64_t encodeCpuNs = 0, encodeCpuUnavailable = 0;
    uint64_t allIndirectDraws = 0, allIndirectDispatches = 0;
};
struct FrameRecord {
    uint64_t id = 0, buffers = 0, gpuUnionNs = 0, gpuSumNs = 0, unavailable = 0, errors = 0, dropped = 0;
    FrameValues values;
};
class FrameAssembler {
    struct Pending {
        uint64_t buffers = 0, outstanding = 0, sum = 0, unavailable = 0, errors = 0;
        bool sealed = false;
        FrameValues values;
        std::vector<GPUInterval> intervals;
    };
    size_t limit;
    uint64_t current = 1, droppedFrames = 0;
    std::map<uint64_t, Pending> frames;
    std::optional<FrameRecord> finish(uint64_t id) {
        auto found = frames.find(id);
        if (found == frames.end() || !found->second.sealed || found->second.outstanding) return {};
        auto& f = found->second;
        FrameRecord result{id, f.buffers, gpuIntervalUnion(std::move(f.intervals)), f.sum,
                           f.unavailable, f.errors, droppedFrames, f.values};
        frames.erase(found);
        return result;
    }
    Pending& ensure() {
        auto found = frames.find(current);
        if (found != frames.end()) return found->second;
        if (frames.size() >= limit) { frames.erase(frames.begin()); ++droppedFrames; }
        return frames[current];
    }
public:
    explicit FrameAssembler(size_t capacity = 120): limit(std::max(size_t(1), capacity)) {}
    uint64_t beginBuffer() {
        auto& frame = ensure(); ++frame.buffers; ++frame.outstanding;
        return current;
    }
    std::optional<FrameRecord> seal(FrameValues values) {
        auto& frame = ensure(); frame.values = values; frame.sealed = true;
        return finish(current++);
    }
    std::optional<FrameRecord> complete(uint64_t id, uint64_t begin, uint64_t end, bool success) {
        auto found = frames.find(id);
        if (found == frames.end() || !found->second.outstanding) return {};
        auto& frame = found->second;
        --frame.outstanding;
        if (!success) ++frame.errors;
        if (success && begin && end >= begin) {
            frame.sum += end - begin;
            // Bound trace memory even when a malformed frame creates enormous
            // numbers of buffers. Mark the frame unusable instead of guessing.
            if (frame.intervals.size() < 4096) frame.intervals.push_back({begin, end});
            else ++frame.unavailable;
        } else ++frame.unavailable;
        return finish(id);
    }
    uint64_t currentId() const { return current; }
    uint64_t dropped() const { return droppedFrames; }
};
}
