#include "MVKReplayGPUStageData.h"
#include <cassert>
using namespace mvkreplay;
int main() {
    GPUStageEpochTracker tracker;
    auto first=tracker.begin(1),secondBuffer=tracker.begin(1);
    auto a=tracker.reserve(first),b=tracker.reserve(secondBuffer);
    assert(a&&b&&a->slot==b->slot&&a->index==0&&b->index==4);
    tracker.seal(1);assert(tracker.complete(first));assert(tracker.active()==1);
    assert(tracker.complete(secondBuffer));assert(tracker.active()==0);
    assert(!tracker.complete(secondBuffer));
    // A token obtained before seal can arrive late, and still retires correctly.
    auto late=tracker.begin(1);assert(late->sealed);assert(tracker.reserve(late));
    assert(tracker.complete(late)&&tracker.active()==0);
    std::array<std::shared_ptr<GPUStageEpoch>,8> live;
    for(unsigned i=0;i<8;++i){live[i]=tracker.begin(i+2);assert(live[i]);assert(tracker.reserve(live[i]));}
    assert(!tracker.begin(10));tracker.seal(9);
    for(auto& epoch:live)assert(tracker.complete(epoch));
    assert(tracker.active()==0);
    auto limit=tracker.begin(10);
    for(unsigned i=0;i<1024;++i)assert(tracker.reserve(limit)->index==i*4);
    assert(!tracker.reserve(limit));tracker.complete(limit);tracker.seal(10);
    assert(tracker.active()==0);
    uint64_t ticks[]={10,30,20,40, 25,45,35,55, 15,25,0,0, 50,60,0,0};
    uint8_t kinds[]={0,0,1,2};auto duration=stageDurations(ticks,kinds,4,1,100);
    assert(duration.validPasses==4&&duration.invalidPasses==0);
    assert(duration.vertexTicks==35&&duration.fragmentTicks==35&&duration.computeTicks==10&&duration.blitTicks==10&&duration.unionTicks==50);
    auto stale=stageDurations(ticks,kinds,4,20,100);assert(stale.invalidPasses==2);
    ticks[0]=UINT64_MAX;assert(stageDurations(ticks,kinds,4).invalidPasses==1);
    uint64_t clearOnly[]={100,110,0,0};uint8_t render[]={0};bool drew[]={false};
    auto empty=stageDurations(clearOnly,render,1,90,120,drew);
    assert(empty.emptyStages==1&&empty.invalidPasses==0&&empty.vertexTicks==10);
    drew[0]=true;assert(stageDurations(clearOnly,render,1,90,120,drew).invalidPasses==1);
    GPUComputeWork work;work.add(123,2,3,4,8,1,1,false);
    assert(work.dispatches==1&&work.groups==24&&work.invocations==192&&!work.mixedPrograms&&!work.overflow);
    auto hash=work.sequenceHash;work.add(123,0,0,0,8,1,1,true);
    assert(work.indirect==1&&work.groups==24&&work.sequenceHash!=hash);
    work.add(456,1,1,1,1,1,1,false);assert(work.mixedPrograms);
    work.add(456,UINT64_MAX,2,2,1,1,1,false);assert(work.overflow);
    GPUStageClock before{1000,1,1000,500},after{3000,1,3000,1000};
    assert(stageClockScale(before,after)==4.0);
    after.hostSpanNs=500;assert(!stageClockScale(before,after));
    after.hostSpanNs=1;after.gpuTicks=before.gpuTicks;assert(!stageClockScale(before,after));
    unsigned selected=0;for(unsigned i=2;i<100002;++i)selected+=sampleGPUStageEpoch(i,12345);
    assert(selected>5000&&selected<7500&&sampleGPUStageEpoch(1,12345));
}
