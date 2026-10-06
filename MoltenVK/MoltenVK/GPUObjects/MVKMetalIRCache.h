#pragma once
#include "MVKMetalIRBridge.h"
#include <CommonCrypto/CommonDigest.h>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <pthread/qos.h>
#include <unistd.h>

namespace mvkir {
// This cache owns only immutable metallib bytes, never Vulkan resources.
// Missing/corrupt records are ordinary compiler misses. Persistence is optional.
class DiskCache {
    struct Header {
        char magic[8];
        uint32_t version, execution;
        uint64_t size;
        char entry[256];
        uint32_t group[3];
        uint8_t vertexAttributes[32];
        uint32_t runtimeFlags;
        unsigned char digest[32];
    };
    struct Write { std::string key; Header header{}; std::vector<uint8_t> bytes; };
    std::filesystem::path _directory;
    std::mutex _lock;
    std::condition_variable _ready;
    std::deque<Write> _pending;
    size_t _pendingBytes=0;
    size_t _pendingCount=0;
    bool _stopping=false;
    std::thread _writer;
    static constexpr size_t MaxBytes=16*1024*1024;

    static void checksum(const Header& header,const void* bytes,unsigned char* out) {
        CC_SHA256_CTX hash;CC_SHA256_Init(&hash);
        CC_SHA256_Update(&hash,&header,offsetof(Header,digest));
        CC_SHA256_Update(&hash,bytes,(CC_LONG)header.size);
        CC_SHA256_Final(out,&hash);
    }
    void writeLoop() {
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
        uint64_t sequence=0;
        for(;;) {
            Write work;
            {
                std::unique_lock<std::mutex> lock(_lock);
                _ready.wait(lock,[&]{return !_pending.empty()||(_stopping&&_pendingCount==0);});
                if(_pending.empty())return;
                work=std::move(_pending.front());_pending.pop_front();
            }
            std::filesystem::path temporary;
            try {
                checksum(work.header,work.bytes.data(),work.header.digest);
                auto destination=_directory/(work.key+".mir");
                temporary=_directory/(work.key+".tmp-"+std::to_string(getpid())+"-"+std::to_string(++sequence));
                std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
                file.write((const char*)&work.header,sizeof(work.header));
                file.write((const char*)work.bytes.data(),work.bytes.size());file.close();
                std::error_code error;
                if(file)std::filesystem::rename(temporary,destination,error);
                if(!file||error)std::filesystem::remove(temporary,error);
            } catch(...) {
                if(!temporary.empty()){std::error_code error;std::filesystem::remove(temporary,error);}
            }
            releaseReservation(work.bytes.size());
        }
    }
    void releaseReservation(size_t bytes) {
        std::lock_guard<std::mutex> lock(_lock);
        _pendingBytes-=bytes;--_pendingCount;
        _ready.notify_all();
    }
    DiskCache() noexcept {
      try {
        const char* path=getenv("MELONX_METAL_IR_CACHE");
        if(!path||!*path)return;
        std::error_code error;
        std::filesystem::create_directories(path,error);
        if(error)return;
        _directory=path;_writer=std::thread([this]{writeLoop();});
      } catch(...) {_directory.clear();}
    }
public:
    static DiskCache& get(){static DiskCache cache;return cache;}
    ~DiskCache(){
        {std::lock_guard<std::mutex> lock(_lock);_stopping=true;}_ready.notify_one();
        if(_writer.joinable())_writer.join();
    }
    bool load(const std::string& key,uint32_t execution,MVKMetalIRCompileResult& result) noexcept {
      try {
        if(_directory.empty())return false;
        std::ifstream file(_directory/(key+".mir"),std::ios::binary);
        Header header{};file.read((char*)&header,sizeof(header));
        if(!file||memcmp(header.magic,"MIRLIB01",8)||header.version!=3||header.execution!=execution||!header.size||header.size>MaxBytes||
           !header.entry[0]||!memchr(header.entry,0,sizeof(header.entry)))return false;
        std::unique_ptr<void,decltype(&free)> bytes(malloc(header.size),free);if(!bytes)return false;
        file.read((char*)bytes.get(),header.size);if(!file)return false;
        unsigned char digest[32];checksum(header,bytes.get(),digest);
        if(!file||file.peek()!=std::ifstream::traits_type::eof()||memcmp(digest,header.digest,32))return false;
        result={};result.abiVersion=MVK_METAL_IR_ABI_VERSION;result.metallib=bytes.release();result.metallibSize=header.size;
        memcpy(result.entry,header.entry,sizeof(result.entry));memcpy(result.threadgroupSize,header.group,sizeof(header.group));
        memcpy(result.vertexAttributes,header.vertexAttributes,sizeof(header.vertexAttributes));
        result.runtimeFlags=header.runtimeFlags;
        return true;
      } catch(...) {return false;}
    }
    void store(const std::string& key,uint32_t execution,const MVKMetalIRCompileResult& result) noexcept {
        if(_directory.empty()||!result.metallib||!result.metallibSize||result.metallibSize>MaxBytes)return;
        // Full queues skip persistence, preserving frame/compile progress.
        {
            std::lock_guard<std::mutex> lock(_lock);
            if(_stopping||_pendingCount>=32||_pendingBytes+result.metallibSize>MaxBytes)return;
            ++_pendingCount;_pendingBytes+=result.metallibSize;
        }
      try {
        Write work;work.key=key;memcpy(work.header.magic,"MIRLIB01",8);work.header.version=3;
        work.header.execution=execution;work.header.size=result.metallibSize;
        memcpy(work.header.entry,result.entry,sizeof(work.header.entry));
        memcpy(work.header.group,result.threadgroupSize,sizeof(work.header.group));
        memcpy(work.header.vertexAttributes,result.vertexAttributes,sizeof(work.header.vertexAttributes));
        work.header.runtimeFlags=result.runtimeFlags;
        work.bytes.assign((const uint8_t*)result.metallib,(const uint8_t*)result.metallib+result.metallibSize);
        {std::lock_guard<std::mutex> lock(_lock);_pending.push_back(std::move(work));}
        _ready.notify_one();
      } catch(...) {releaseReservation(result.metallibSize);}
    }
    void invalidate(const std::string& key) noexcept {try {
        if(!_directory.empty()){std::error_code error;std::filesystem::remove(_directory/(key+".mir"),error);}
      } catch(...) {}
    }
};
}
