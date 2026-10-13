#include "MVKMetalIRCache.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <thread>

using mvkir::DiskCache;
namespace fs=std::filesystem;
static std::string key(char value) { return std::string(64,value); }
static MVKMetalIRCompileResult result(std::vector<uint8_t>& bytes) {
    MVKMetalIRCompileResult r{};r.abiVersion=MVK_METAL_IR_ABI_VERSION;
    r.metallib=bytes.data();r.metallibSize=bytes.size();strcpy(r.entry,"main");
    r.threadgroupSize[0]=8;r.threadgroupSize[1]=r.threadgroupSize[2]=1;
    r.runtimeFlags=MVK_METAL_IR_RUNTIME_DATA; r.vertexAttributes[3]=7;return r;
}
static bool load(DiskCache& cache,const std::string& k,DiskCache::Reflection* out=nullptr) {
    MVKMetalIRCompileResult r{};DiskCache::Reflection reflection;
    bool ok=cache.load(k,5,r,reflection);
    if(ok) { assert(r.abiVersion==MVK_METAL_IR_ABI_VERSION&&r.threadgroupSize[0]==8&&r.vertexAttributes[3]==7);
        assert(r.metallibSize==1024&&static_cast<uint8_t*>(r.metallib)[0]==42);free(r.metallib); }
    if(out)*out=reflection;return ok;
}
static void patch(const fs::path& path,std::streamoff offset,uint32_t value) {
    std::fstream f(path,std::ios::in|std::ios::out|std::ios::binary);f.seekp(offset);f.write((char*)&value,4);assert(f.good());
}
int main(int argc,char** argv) {
    assert(argc==2);fs::path root=argv[1];assert(!fs::exists(root));fs::create_directories(root);
    std::vector<uint8_t> bytes(1024,42);auto r=result(bytes);
    DiskCache::Reflection metadata;metadata.usedSets=5;metadata.vertexLocations=8;
    metadata.usedBindings={1,(2ull<<32)|3};metadata.usesPushConstants=true;metadata.usesPointCoordinates=true;
    {
        DiskCache cache(root/"roundtrip",1024*1024);assert(cache.enabled());
        cache.store(key('a'),5,r,metadata);cache.waitIdle();DiskCache::Reflection restored;
        assert(load(cache,key('a'),&restored));assert(restored.usedSets==5&&restored.vertexLocations==8);
        assert(restored.usedBindings==metadata.usedBindings&&restored.usesPushConstants&&restored.usesPointCoordinates);
        auto path=root/"roundtrip"/(key('a')+".mir");
        // Any altered payload is deleted and becomes an IR compiler miss.
        patch(path,fs::file_size(path)-4,99);assert(!load(cache,key('a'))&&!fs::exists(path));
        cache.store(key('a'),5,r,metadata);cache.waitIdle();fs::resize_file(path,10);
        assert(!load(cache,key('a'))&&!fs::exists(path));
        cache.store(key('a'),5,r,metadata);cache.waitIdle();patch(path,8,999);
        assert(!load(cache,key('a'))&&!fs::exists(path));
        cache.store(key('a'),5,r,metadata);cache.waitIdle();patch(path,12,999);
        assert(!load(cache,key('a'))&&!fs::exists(path));
        cache.store(key('a'),5,r,metadata);cache.waitIdle();
        {std::ofstream f(path,std::ios::binary|std::ios::app);f.put('x');}
        assert(!load(cache,key('a'))&&!fs::exists(path));
        cache.store("../escape",5,r,metadata);cache.waitIdle();assert(!fs::exists(root/"escape.mir"));
        std::thread first([&]{for(int i=0;i<16;++i)cache.store(key('b'),5,r,metadata);});
        std::thread second([&]{for(int i=0;i<16;++i)cache.store(key('b'),5,r,metadata);});
        first.join();second.join();cache.waitIdle();assert(load(cache,key('b')));
        for(auto& e:fs::directory_iterator(root/"roundtrip"))assert(e.path().extension()==".mir");
    }
    {
        // Fit two records; a successful read must retain a over untouched b.
        DiskCache cache(root/"lru",3000);
        cache.store(key('a'),5,r,metadata);cache.store(key('b'),5,r,metadata);cache.waitIdle();
        assert(load(cache,key('a')));cache.store(key('c'),5,r,metadata);cache.waitIdle();
        assert(load(cache,key('a'))&&!load(cache,key('b'))&&load(cache,key('c')));
        uintmax_t total=0;for(auto& e:fs::directory_iterator(root/"lru"))total+=e.file_size();assert(total<=3000);
    }
    {
        // Access order survives reopening, not just the in-memory LRU list.
        DiskCache cache(root/"lru",3000);cache.store(key('d'),5,r,metadata);cache.waitIdle();
        assert(!load(cache,key('a'))&&load(cache,key('c'))&&load(cache,key('d')));
    }
    {
        // A payload at the byte limit plus its header cannot fit in the write
        // queue. This deterministically exercises pressure without racing I/O.
        DiskCache cache(root/"pressure",0);
        std::vector<uint8_t> large(16*1024*1024,42);auto oversized=result(large);
        cache.store(key('e'),5,oversized,metadata);cache.waitIdle();
        assert(cache.droppedWrites()==1&&cache.failedWrites()==0);
        assert(!load(cache,key('e')));
        cache.store(key('a'),5,r,metadata);cache.waitIdle();
        assert(load(cache,key('a'))&&cache.droppedWrites()==1);
    }
    {
        // Rename onto a directory must fail; the writer must release its
        // reservation, report failure, and continue accepting valid writes.
        auto directory=root/"write-failure";fs::create_directories(directory/(key('f')+".mir"));
        DiskCache cache(directory,0);
        cache.store(key('f'),5,r,metadata);cache.waitIdle();
        assert(cache.failedWrites()==1&&cache.droppedWrites()==0);
        cache.store(key('a'),5,r,metadata);cache.waitIdle();assert(load(cache,key('a')));
        for(auto& e:fs::directory_iterator(directory))assert(e.path().extension()==".mir");
    }
    DiskCache disabled({},0);assert(!disabled.enabled());disabled.store(key('a'),5,r,metadata);
    assert(!load(disabled,key('a')));printf("disk cache: roundtrip, corruption, truncation, version, ABI, trailing bytes, concurrent writes, LRU, restart, pressure, failed-write recovery, disabled and key validation passed\n");
}
