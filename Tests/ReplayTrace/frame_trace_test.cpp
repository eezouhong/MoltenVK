#include "MVKReplayFrameTrace.h"
#include <cassert>
#include <vector>
using namespace mvkreplay;
int main() {
    assert(gpuIntervalUnion({{20,40},{10,30}}) == 30);
    assert(gpuIntervalUnion({{10,20},{30,40},{12,18}}) == 20);
    assert(gpuIntervalUnion({{10,10}}) == 0);
    FrameAssembler frames(3);
    auto first = frames.beginBuffer();
    auto second = frames.beginBuffer();
    FrameValues values{}; values.renderEncoders = 4;
    assert(!frames.seal(values));
    assert(!frames.complete(first, 10, 30, true));
    auto third = frames.beginBuffer();
    assert(third != first);
    auto ready = frames.complete(second, 20, 40, true);
    assert(ready && ready->buffers == 2 && ready->gpuUnionNs == 30 && ready->gpuSumNs == 40);
    assert(ready->values.renderEncoders == 4);
    assert(!frames.complete(third, 50, 60, true));
    ready = frames.seal({});
    assert(ready && ready->buffers == 1 && ready->gpuUnionNs == 10);
    auto bad = frames.beginBuffer();
    assert(!frames.seal({}));
    ready = frames.complete(bad, 0, 0, false);
    assert(ready && ready->errors == 1 && ready->unavailable == 1);
    auto missing = frames.beginBuffer();
    assert(!frames.seal({}));
    ready = frames.complete(missing, 0, 0, true);
    assert(ready && ready->errors == 0 && ready->unavailable == 1);
    FrameAssembler bounded(1);
    auto old = bounded.beginBuffer();
    assert(!bounded.seal({}));
    auto newer = bounded.beginBuffer();
    assert(bounded.dropped() == 1 && newer != old);
    assert(!bounded.complete(old, 10, 20, true));
    assert(!bounded.complete(newer, 20, 30, true));
    ready = bounded.seal({});
    assert(ready && ready->dropped == 1);
}
