#include "MVKReplayIRTrace.h"
#include <cassert>

extern "C" void disabled_ir_byte_hook(uint64_t bytes) {
    mvkreplay::runtimeParameterBytes(bytes);
}

#ifndef TEST_DISABLED_OBJECT
int main() {
#if MVK_REPLAY_TRACE
    using namespace mvkreplay;
    gpu.irTemporaryBytes.store(0);
    setMode(Mode::Off);
    runtimeParameterBytes(17);
    setMode(Mode::Detailed);
    runtimeParameterBytes(29);
    assert(gpu.irTemporaryBytes.load() == 0);
    setMode(Mode::Coarse);
    runtimeParameterBytes(16);
    runtimeParameterBytes(48);
    runtimeParameterBytes(0); // Raw aliases do not copy CPU payload.
    SubmissionSample sample{};
    assert(submissionSnapshot(&sample) && sample.temporaryBytes == 64);
    setMode(Mode::Off);
    runtimeParameterBytes(128);
    assert(gpu.irTemporaryBytes.load() == 64);
#else
    disabled_ir_byte_hook(64);
    assert(!mvkreplay::enabled());
#endif
}
#endif
