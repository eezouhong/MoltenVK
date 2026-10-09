#pragma once
#include "MVKMetalIRBridge.h"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <pthread/qos.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mvkir {
// Device-owned immutable bytes and reflection. Cache failure is an IR compiler
// miss. No Vulkan/Metal object is retained, and writes run on a utility thread.
class DiskCache {
public:
    struct Reflection {
        uint64_t usedSets=0, vertexLocations=0;
        bool usesPushConstants=false, usesPointCoordinates=false;
        std::vector<uint64_t> usedBindings;
    };
private:
    struct Header {
        char magic[8];
        uint32_t version, abi, execution, bindingCount;
        uint64_t size;
        char entry[256];
        uint32_t group[3];
        uint8_t vertexAttributes[32];
        uint32_t runtimeFlags, usageFlags;
        uint64_t usedSets, vertexLocations;
        unsigned char digest[32];
    };
    struct Write { std::string key; Header header{}; std::vector<uint64_t> bindings; std::vector<uint8_t> bytes; };
    struct Entry { uint64_t bytes; std::list<std::string>::iterator age; };
    std::filesystem::path _directory;
    uint64_t _maxBytes=0, _diskBytes=0;
    std::mutex _lock;
    std::condition_variable _workReady, _idle;
    // Filesystem mutation never holds the reservation/index lock. Valid loads
    // perform their reads without either lock.
    std::mutex _fileLock;
    std::atomic<uint64_t> _droppedWrites{0}, _failedWrites{0};
    std::deque<Write> _pending;
    std::list<std::string> _lru;
    std::unordered_map<std::string,Entry> _entries;
    size_t _pendingBytes=0, _pendingCount=0;
    bool _stopping=false;
    std::thread _writer;
    static constexpr size_t MaxBytes=16*1024*1024, MaxBindings=4096;
    inline static std::atomic<uint64_t> temporarySequence{0};

    static bool validKey(const std::string& key) {
        return key.size()==64&&std::all_of(key.begin(),key.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
    }
    static void checksum(const Header& header,const std::vector<uint64_t>& bindings,const void* bytes,unsigned char* out) {
        CC_SHA256_CTX hash;CC_SHA256_Init(&hash);
        CC_SHA256_Update(&hash,&header,offsetof(Header,digest));
        CC_SHA256_Update(&hash,bindings.data(),(CC_LONG)(bindings.size()*sizeof(uint64_t)));
        CC_SHA256_Update(&hash,bytes,(CC_LONG)header.size);CC_SHA256_Final(out,&hash);
    }
    void forget(const std::string& key) {
        auto found=_entries.find(key);if(found==_entries.end())return;
        _diskBytes-=found->second.bytes;_lru.erase(found->second.age);_entries.erase(found);
    }
    void remember(const std::string& key,uint64_t bytes) {
        forget(key);_lru.push_back(key);
        try {_entries.emplace(key,Entry{bytes,std::prev(_lru.end())});}
        catch(...) {_lru.pop_back();throw;}
        _diskBytes+=bytes;
    }
    // Caller serializes file mutations; each victim is detached under the
    // short index lock before deletion, so no compiler waits for rename/trim.
    void trim() {
        for (;;) {
            std::string key;
            {
                std::lock_guard<std::mutex> lock(_lock);
                if (!_maxBytes || _diskBytes<=_maxBytes || _lru.empty()) return;
                key=_lru.front();forget(key);
            }
            std::error_code error;
            std::filesystem::remove(_directory/(key+".mir"),error);
        }
    }
    void discardInvalid(const std::string& key,const struct stat& opened) {
        std::lock_guard<std::mutex> files(_fileLock);struct stat current{};
        auto path=_directory/(key+".mir");
        // Do not delete a new valid atomic replacement of the file we read.
        if(!lstat(path.c_str(),&current)&&current.st_dev==opened.st_dev&&current.st_ino==opened.st_ino) {
            std::error_code error;std::filesystem::remove(path,error);
            std::lock_guard<std::mutex> lock(_lock);forget(key);
        }
    }
    void releaseReservation(size_t bytes) {
        std::lock_guard<std::mutex> lock(_lock);_pendingBytes-=bytes;--_pendingCount;
        if(!_pendingCount) _idle.notify_all();
        if(_stopping) _workReady.notify_all();
    }
    void writeLoop() {
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
        for(;;) {
            Write work;
            {
                std::unique_lock<std::mutex> lock(_lock);
                _workReady.wait(lock,[&]{return !_pending.empty()||(_stopping&&_pendingCount==0);});
                if(_pending.empty())return;work=std::move(_pending.front());_pending.pop_front();
            }
            std::filesystem::path temporary;
            try {
                checksum(work.header,work.bindings,work.bytes.data(),work.header.digest);
                auto destination=_directory/(work.key+".mir");
                temporary=_directory/(work.key+".tmp-"+std::to_string(getpid())+"-"+std::to_string(++temporarySequence));
                int descriptor=open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL,0600);
                FILE* file=descriptor<0?nullptr:fdopen(descriptor,"wb");bool good=false;
                if(descriptor>=0&&!file)close(descriptor);
                if(file) {
                    good=fwrite(&work.header,1,sizeof(work.header),file)==sizeof(work.header);
                    good=good&&fwrite(work.bindings.data(),sizeof(uint64_t),work.bindings.size(),file)==work.bindings.size();
                    good=good&&fwrite(work.bytes.data(),1,work.bytes.size(),file)==work.bytes.size();
                    good=fclose(file)==0&&good;
                }
                std::error_code error;
                {
                    std::lock_guard<std::mutex> files(_fileLock);
                    if(good)std::filesystem::rename(temporary,destination,error);
                    if(good&&!error) {
                        {
                            std::lock_guard<std::mutex> lock(_lock);
                            remember(work.key,sizeof(Header)+work.bindings.size()*8+work.bytes.size());
                        }
                        trim();
                    } else {
                        ++_failedWrites;
                        std::filesystem::remove(temporary,error);
                    }
                }
            } catch(...) {
                ++_failedWrites;
                if(!temporary.empty()){std::error_code error;std::filesystem::remove(temporary,error);}
            }
            releaseReservation(work.bytes.size()+work.bindings.size()*8+sizeof(Header));
        }
    }
public:
    explicit DiskCache(std::filesystem::path directory,uint64_t maxBytes=2ull*1024*1024*1024) noexcept : _maxBytes(maxBytes) {
      try {
        if(directory.empty())return;std::error_code error;
        std::filesystem::create_directories(directory,error);if(error)return;
        if(std::filesystem::is_symlink(std::filesystem::symlink_status(directory,error))||error)return;
        _directory=std::move(directory);
        struct Existing { std::string key;uint64_t size;std::filesystem::file_time_type age; };
        std::vector<Existing> files;
        for(const auto& file:std::filesystem::directory_iterator(_directory)) {
            auto stem=file.path().stem().string();
            if(file.path().extension()!=".mir"||!validKey(stem)||file.is_symlink()||!file.is_regular_file())continue;
            files.push_back({stem,file.file_size(),file.last_write_time()});
        }
        std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.age<b.age;});
        for(const auto& file:files)remember(file.key,file.size);trim();
        _writer=std::thread([this]{writeLoop();});
      } catch(...) {_directory.clear();}
    }
    uint64_t droppedWrites() const {return _droppedWrites.load(std::memory_order_relaxed);}
    uint64_t failedWrites() const {return _failedWrites.load(std::memory_order_relaxed);}
    bool enabled() const {return !_directory.empty();}
    ~DiskCache() {
        {std::lock_guard<std::mutex> lock(_lock);_stopping=true;}_workReady.notify_one();
        if(_writer.joinable())_writer.join();
    }
    void waitIdle() {
        std::unique_lock<std::mutex> lock(_lock);_idle.wait(lock,[&]{return _pendingCount==0;});
    }
    bool load(const std::string& key,uint32_t execution,MVKMetalIRCompileResult& result,Reflection& reflection) noexcept {
      try {
        if(_directory.empty()||!validKey(key))return false;
        auto path=_directory/(key+".mir");struct stat pathStat{};
        if(lstat(path.c_str(),&pathStat)||!S_ISREG(pathStat.st_mode))return false;
        int descriptor=open(path.c_str(),O_RDONLY|O_NOFOLLOW);if(descriptor<0)return false;
        FILE* stream=fdopen(descriptor,"rb");if(!stream){close(descriptor);return false;}
        std::unique_ptr<FILE,decltype(&fclose)> file(stream,fclose);
        struct stat opened{};if(fstat(fileno(file.get()),&opened))return false;
        auto invalid=[&]{discardInvalid(key,opened);return false;};
        Header header{};
        if(fread(&header,1,sizeof(header),file.get())!=sizeof(header)||memcmp(header.magic,"MIRLIB02",8)||
           header.version!=4||header.abi!=MVK_METAL_IR_ABI_VERSION||header.execution!=execution||
           !header.size||header.size>MaxBytes||header.bindingCount>MaxBindings||
           !header.entry[0]||!memchr(header.entry,0,sizeof(header.entry))||(header.usageFlags&~3u))return invalid();
        std::vector<uint64_t> bindings(header.bindingCount);
        std::unique_ptr<void,decltype(&free)> bytes(malloc(header.size),free);if(!bytes)return false;
        if(fread(bindings.data(),sizeof(uint64_t),bindings.size(),file.get())!=bindings.size()||
           fread(bytes.get(),1,header.size,file.get())!=header.size||fgetc(file.get())!=EOF||ferror(file.get()))return invalid();
        unsigned char digest[32];checksum(header,bindings,bytes.get(),digest);
        if(memcmp(digest,header.digest,32))return invalid();
        reflection.usedSets=header.usedSets;reflection.vertexLocations=header.vertexLocations;reflection.usedBindings=std::move(bindings);
        reflection.usesPushConstants=header.usageFlags&1;reflection.usesPointCoordinates=header.usageFlags&2;
        // Touch the opened inode, not a possibly newer atomic replacement.
        // Do not resurrect a trimmed key in the index after a concurrent read.
        timespec times[2]={{0,UTIME_OMIT},{0,UTIME_NOW}};
        futimens(fileno(file.get()),times);
        {
            std::lock_guard<std::mutex> lock(_lock);
            auto found=_entries.find(key);
            if(found!=_entries.end()) _lru.splice(_lru.end(),_lru,found->second.age);
        }
        result={};result.abiVersion=header.abi;result.metallib=bytes.release();result.metallibSize=header.size;
        memcpy(result.entry,header.entry,sizeof(result.entry));memcpy(result.threadgroupSize,header.group,sizeof(header.group));
        memcpy(result.vertexAttributes,header.vertexAttributes,sizeof(header.vertexAttributes));result.runtimeFlags=header.runtimeFlags;
        return true;
      } catch(...) {return false;}
    }
    void store(const std::string& key,uint32_t execution,const MVKMetalIRCompileResult& result,const Reflection& reflection) noexcept {
        if(_directory.empty()||!validKey(key)||result.abiVersion!=MVK_METAL_IR_ABI_VERSION||!result.metallib||
           !result.metallibSize||result.metallibSize>MaxBytes||reflection.usedBindings.size()>MaxBindings)return;
        const size_t bytes=result.metallibSize+reflection.usedBindings.size()*8+sizeof(Header);
        if(_maxBytes&&bytes>_maxBytes)return;
        {
            std::lock_guard<std::mutex> lock(_lock);
            if(_stopping||_pendingCount>=32||_pendingBytes+bytes>MaxBytes){++_droppedWrites;return;}
            ++_pendingCount;_pendingBytes+=bytes;
        }
      try {
        Write work;work.key=key;memcpy(work.header.magic,"MIRLIB02",8);work.header.version=4;
        work.header.abi=result.abiVersion;work.header.execution=execution;work.header.size=result.metallibSize;
        memcpy(work.header.entry,result.entry,sizeof(result.entry));memcpy(work.header.group,result.threadgroupSize,sizeof(work.header.group));
        memcpy(work.header.vertexAttributes,result.vertexAttributes,sizeof(work.header.vertexAttributes));work.header.runtimeFlags=result.runtimeFlags;
        work.header.usedSets=reflection.usedSets;work.header.vertexLocations=reflection.vertexLocations;
        work.header.usageFlags=uint32_t(reflection.usesPushConstants)|(uint32_t(reflection.usesPointCoordinates)<<1);
        work.header.bindingCount=reflection.usedBindings.size();work.bindings=reflection.usedBindings;
        work.bytes.assign((const uint8_t*)result.metallib,(const uint8_t*)result.metallib+result.metallibSize);
        {std::lock_guard<std::mutex> lock(_lock);_pending.push_back(std::move(work));}_workReady.notify_one();
      } catch(...) {++_failedWrites;releaseReservation(bytes);}
    }
    void invalidate(const std::string& key) noexcept {try {
        if(!_directory.empty()&&validKey(key)) {
            std::lock_guard<std::mutex> files(_fileLock);std::error_code error;
            std::filesystem::remove(_directory/(key+".mir"),error);
            std::lock_guard<std::mutex> lock(_lock);forget(key);
        }
      } catch(...) {}
    }
};
}
