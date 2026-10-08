#include "MVKReplayTrace.h"
#include "MVKReplayBindingTrace.h"
#include <cstdint>
extern "C" uint64_t disabled_trace_hot_path() {
    mvkreplay::Timer timer(mvkreplay::MetalCommandEncoding);
    mvkreplay::BindingTrace binding(true,true,mvkreplay::BindingGroup::Resources);
    binding.checkpoint();binding.checkpoint();
    mvkreplay::DescriptorRangeTimer range(mvkreplay::DescriptorUpdate);
    mvkreplay::DescriptorBatchTrace batch(true);
    mvkreplay::DescriptorSampleScope scope(mvkreplay::DescriptorRangeKind::Write,true);
    mvkreplay::indirectRuntime(7,64,true,false);
    mvkreplay::indirectInvocation(7,false);
    mvkreplay::encoderStarted(0);
    return uint64_t(mvkreplay::enabled()) + mvkreplay::framePresented() +
        mvkreplay::snapshot(nullptr,0,false) + mvkreplay::bindingSnapshot(nullptr,0);
}
int main() { return disabled_trace_hot_path()==0 ? 0 : 1; }
