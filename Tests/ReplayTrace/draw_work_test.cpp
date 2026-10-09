#include "MVKReplayDrawWork.h"
#include <cassert>
int main() {
    mvkreplay::GPUDrawWork work;
    work.add(11,22,3,2,false,true);
    assert(work.draws==1&&work.inputElements==6&&work.unknownDraws==0&&!work.mixedPrograms);
    auto sequence=work.sequenceHash;
    work.add(11,22,6,3,true,true);
    assert(work.inputElements==24&&work.indexedDraws==1&&work.sequenceHash!=sequence);
    // Never guess a count for indirect or implementation-expanded draws.
    work.add(11,22,0,0,true,false);
    assert(work.inputElements==24&&work.unknownDraws==1&&work.indexedDraws==2);
    work.add(12,22,0,99,false,true);
    assert(work.mixedPrograms&&work.inputElements==24);
    work.add(11,22,UINT64_MAX,2,false,true);assert(work.overflow);
    mvkreplay::GPUDrawWork sum;
    sum.add(1,2,UINT64_MAX,1,false,true);
    sum.add(1,2,1,1,false,true);assert(sum.overflow&&sum.inputElements==UINT64_MAX);
}
