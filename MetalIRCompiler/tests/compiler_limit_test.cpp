#include "../../MoltenVK/MoltenVK/GPUObjects/MVKMetalIRCompilerLimit.h"
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

int main() {
    mvkir::CompilerLimit limit(2);
    std::atomic<unsigned> started{0},inside{0},completed{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;
    for (unsigned i=0;i<8;++i) threads.emplace_back([&] {
        ++started;
        while(started!=8)std::this_thread::yield();
        mvkir::CompilerLimit::Permit permit(limit);
        ++inside;
        while(!release)std::this_thread::yield();
        --inside;++completed;
    });
    while(limit.stats().active!=2 || limit.stats().waiting!=6)std::this_thread::yield();
    unsigned relief=0;
    assert(!limit.relieveWhenIdle(std::chrono::milliseconds(0),[&]{++relief;}));
    release=true;
    for(auto& t:threads)t.join();
    assert(completed==8 && inside==0 && limit.stats().peak==2);
    assert(limit.relieveWhenIdle(std::chrono::milliseconds(0),[&]{++relief;}));
    assert(!limit.relieveWhenIdle(std::chrono::milliseconds(0),[&]{++relief;}));
    assert(relief==1 && limit.stats().reliefs==1);
    try { mvkir::CompilerLimit::Permit permit(limit);throw 7; }catch(int){}
    assert(limit.stats().active==0);
    assert(limit.relieveWhenIdle(std::chrono::milliseconds(0),[&]{++relief;}));
    assert(relief==2);
}
