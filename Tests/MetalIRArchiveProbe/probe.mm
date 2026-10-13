#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <atomic>
#include <thread>
#include <unistd.h>
#include "CapturedRender.h"

// Standalone evidence fixture. Does not link MoltenVK or alter any product route.
static uint64_t ns() {
    static mach_timebase_info_data_t b = [] { mach_timebase_info_data_t v; mach_timebase_info(&v); return v; }();
    return (uint64_t)((__uint128_t)mach_absolute_time() * b.numer / b.denom);
}
static NSNumber* footprint() {
    task_vm_info_data_t info{}; mach_msg_type_number_t count=TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(),TASK_VM_INFO,(task_info_t)&info,&count)==KERN_SUCCESS ? @(info.phys_footprint) : nil;
}
static NSDictionary* failure(NSError* error) {
    return error ? @{ @"domain":error.domain,@"code":@(error.code),@"message":error.localizedDescription }
                 : @{ @"message":@"nil result without NSError" };
}
static id<MTLLibrary> load(id<MTLDevice> device, NSString* path, NSMutableArray* loads) {
    uint64_t start=ns(); NSData* bytes=[NSData dataWithContentsOfFile:path]; uint64_t io=ns()-start;
    if(!bytes)return nil;
    dispatch_data_t data=dispatch_data_create(bytes.bytes,bytes.length,nullptr,DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError* error=nil; start=ns(); id<MTLLibrary> lib=[device newLibraryWithData:data error:&error];
    NSMutableDictionary* row=[@{@"path":path,@"bytes":@(bytes.length),@"readNs":@(io),@"newLibraryWithDataNs":@(ns()-start),@"success":@(lib!=nil)} mutableCopy];
    if(!lib)row[@"error"]=failure(error); [loads addObject:row]; return lib;
}
static id<MTLRenderPipelineState> pipeline(id<MTLDevice> device, MTLRenderPipelineDescriptor* d,
                                         id<MTLBinaryArchive> archive, bool requireHit, NSError** error) {
    d.binaryArchives=archive ? @[archive] : @[];
    return [device newRenderPipelineStateWithDescriptor:d
        options:requireHit ? MTLPipelineOptionFailOnBinaryArchiveMiss : MTLPipelineOptionNone reflection:nil error:error];
}
static NSDictionary* flush(id<MTLBinaryArchive> archive, NSString* path, std::atomic<bool>* go=nullptr) {
    __block NSDictionary* result=nil;
    dispatch_semaphore_t done=dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_BACKGROUND,0), ^{
        @autoreleasepool {
            NSError* error=nil; uint64_t tid=0; pthread_threadid_np(nullptr,&tid); uint64_t start=ns();
            if(go)go->store(true,std::memory_order_release);
            BOOL ok=[archive serializeToURL:[NSURL fileURLWithPath:path] error:&error]; uint64_t end=ns();
            NSDictionary* attrs=[[NSFileManager defaultManager] attributesOfItemAtPath:path error:nil];
            NSMutableDictionary* row=[@{@"startNs":@(start),@"endNs":@(end),@"durationNs":@(end-start),
                @"success":@(ok),@"threadId":@(tid),@"mainThread":@([NSThread isMainThread]),
                @"requestedQoS":@"background",@"archiveBytes":attrs[NSFileSize] ?: @0} mutableCopy];
            if(!ok)row[@"error"]=failure(error); result=row;
        }
        dispatch_semaphore_signal(done);
    });
    dispatch_semaphore_wait(done,DISPATCH_TIME_FOREVER);
    return result;
}
int main(int argc,const char* argv[]) {
    @autoreleasepool {
        if(argc==2 && !strcmp(argv[1],"paths")) {
            char cache[4096]={};confstr(_CS_DARWIN_USER_CACHE_DIR,cache,sizeof(cache));
            NSDictionary* paths=@{@"darwinUserCacheDirectory":@(cache),@"homeDirectory":NSHomeDirectory(),
                @"bundleIdentifier":[NSBundle mainBundle].bundleIdentifier ?: @"unbundled"};
            puts([[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:paths options:0 error:nil] encoding:NSUTF8StringEncoding].UTF8String);
            return 0; // Foundation metadata only; no Metal device or graphics lease.
        }
        if(argc!=3 || !getenv("MELONX_DEBUG_GRAPHICS_LEASE"))return 64;
        alarm(180);
        NSDictionary* config=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[1])] options:0 error:nil];
        NSArray* inputs=[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:config[@"inputs"]] options:0 error:nil];
        NSString* mode=config[@"mode"]; NSUInteger count=[config[@"count"] unsignedIntegerValue];
        const NSUInteger workers=32;
        if(!inputs || count==0 || inputs.count<count+workers)return 64;
        id<MTLDevice> device=MTLCreateSystemDefaultDevice(); if(!device)return 69;
        NSMutableArray* rows=[NSMutableArray new],*loads=[NSMutableArray new],*memory=[NSMutableArray new];
        char cacheDirectory[4096]={};confstr(_CS_DARWIN_USER_CACHE_DIR,cacheDirectory,sizeof(cacheDirectory));
        NSMutableDictionary* result=[@{@"schemaVersion":@1,@"mode":mode,@"count":@(count),@"device":device.name,
            @"os":[NSProcessInfo processInfo].operatingSystemVersionString,@"bundleIdentifier":[NSBundle mainBundle].bundleIdentifier ?: @"unbundled",
            @"darwinUserCacheDirectory":@(cacheDirectory),@"rows":rows,@"loads":loads,@"memory":memory,
            @"scope":@"legacy Metal render PSO/archive diagnostic; no draw, dispatch, game or product implementation",
            @"sourceCaptureComplete":@NO,@"metal4APIsCalled":@NO,
            @"libraryPolicy":@"retain all cohort libraries; not a product IR512 test",
            @"footprintBeforeBytes":footprint() ?: (id)[NSNull null]} mutableCopy];
        NSMutableDictionary* libraries=[NSMutableDictionary new]; NSMutableArray* descriptors=[NSMutableArray new];
        bool failed=false;
        for(NSUInteger i=0;i<count+workers;i++) {
            NSDictionary* input=inputs[i];
            for(NSString* role in @[@"vertex",@"fragment"]) {
                NSString* path=input[role]; if(!path || (id)path==[NSNull null] || libraries[path])continue;
                id<MTLLibrary> lib=load(device,path,loads); if(!lib){failed=true;break;} libraries[path]=lib;
            }
            if(failed)break;
            bool hasFragment=input[@"fragment"] && (id)input[@"fragment"]!=[NSNull null];
            MTLRenderPipelineDescriptor* d=restoreRender(input,libraries[input[@"vertex"]],hasFragment ? libraries[input[@"fragment"]] : nil);
            if(!d.vertexFunction || (hasFragment && !d.fragmentFunction)){failed=true;break;}
            [descriptors addObject:d];
        }
        result[@"footprintAfterLibrariesBytes"]=footprint() ?: (id)[NSNull null];
        NSError* error=nil; id<MTLBinaryArchive> archive=nil;
        bool capture=[mode isEqual:@"capture"] || [mode hasPrefix:@"concurrent-"];
        bool lookup=[mode isEqual:@"lookup"] || [mode isEqual:@"miss"] || [mode isEqual:@"corrupt"];
        if(!failed && (capture || lookup)) {
            MTLBinaryArchiveDescriptor* d=[MTLBinaryArchiveDescriptor new];
            if(lookup)d.url=[NSURL fileURLWithPath:config[@"archive"]];
            uint64_t start=ns(); archive=[device newBinaryArchiveWithDescriptor:d error:&error];
            result[@"archiveOpenNs"]=@(ns()-start);result[@"archiveOpened"]=@(archive!=nil);
            if(!archive){result[@"archiveOpenError"]=failure(error);if(![mode isEqual:@"corrupt"])failed=true;}
        }
        if(!failed && ![mode isEqual:@"corrupt"]) {
            NSUInteger n=[mode isEqual:@"miss"] ? 1:count;
            for(NSUInteger i=0;i<n;i++) {
                @autoreleasepool {
                    NSUInteger index=[mode isEqual:@"miss"] ? count:i;
                    MTLRenderPipelineDescriptor* d=descriptors[index];error=nil;
                    NSMutableDictionary* row=[@{@"index":@(index),@"descriptorId":inputs[index][@"descriptorId"]} mutableCopy];
                    if(capture){
                        uint64_t start=ns();BOOL ok=[archive addRenderPipelineFunctionsWithDescriptor:d error:&error];
                        row[@"addNs"]=@(ns()-start);row[@"addSuccess"]=@(ok);
                        if(!ok){failed=true;row[@"addError"]=failure(error);}
                    }
                    if(![mode hasPrefix:@"concurrent-"]) {
                        uint64_t start=ns();id<MTLRenderPipelineState> p=pipeline(device,d,archive,lookup,&error);
                        row[@"pipelineNs"]=@(ns()-start);row[@"pipelineSuccess"]=@(p!=nil);
                        if([mode isEqual:@"miss"]){
                            row[@"missDetected"]=@(p==nil); if(p)failed=true; else row[@"missError"]=failure(error);
                            start=ns();p=pipeline(device,d,archive,false,&error);
                            row[@"fallbackNs"]=@(ns()-start);row[@"fallbackSuccess"]=@(p!=nil);if(!p)failed=true;
                        } else if(!p){failed=true;row[@"error"]=failure(error);}
                    }
                    [rows addObject:row];
                    if(i==0 || (i+1)%50==0 || i+1==n)
                        [memory addObject:@{@"count":@(i+1),@"footprintBytes":footprint() ?: (id)[NSNull null]}];
                }
                if(failed)break;
            }
        }
        if(!failed && [mode isEqual:@"capture"]) {
            result[@"flush"]=flush(archive,config[@"archive"]);
            if(![result[@"flush"][@"success"] boolValue])failed=true;
            result[@"footprintAfterFlushBytes"]=footprint() ?: (id)[NSNull null];
            // MTLBinaryArchive serialization retains its object; measure a second serialization too.
            // Changing serialize destinations relocated the first file on this host.
            // Reuse one URL; do not invalidate the next process's load path.
            result[@"secondFlush"]=flush(archive,config[@"archive"]);
            if(![result[@"secondFlush"][@"success"] boolValue])failed=true;
        }
        if(!failed && [mode hasPrefix:@"concurrent-"]) {
            std::atomic<bool> ready{false},go{false}; NSMutableArray* workerRows=[NSMutableArray new];
            std::thread worker([&]{
                @autoreleasepool {
                    ready.store(true,std::memory_order_release);
                    while(!go.load(std::memory_order_acquire)) std::this_thread::yield();
                    uint64_t tid=0;pthread_threadid_np(nullptr,&tid);
                    for(NSUInteger j=0;j<workers;j++) {
                        @autoreleasepool {
                            NSError* e=nil;uint64_t start=ns();id<MTLRenderPipelineState> p=pipeline(device,descriptors[count+j],nil,false,&e);uint64_t end=ns();
                            NSMutableDictionary* row=[@{@"index":@(count+j),@"startNs":@(start),@"endNs":@(end),@"pipelineNs":@(end-start),@"success":@(p!=nil),@"threadId":@(tid)} mutableCopy];
                            if(!p)row[@"error"]=failure(e);[workerRows addObject:row];
                        }
                    }
                }
            });
            while(!ready.load(std::memory_order_acquire))std::this_thread::yield();
            NSDictionary* flushRow=nil;
            if([mode isEqual:@"concurrent-flush"]) {
                flushRow=flush(archive,config[@"archive"],&go);
            } else {go.store(true,std::memory_order_release);}
            worker.join(); result[@"workerRows"]=workerRows;
            if(flushRow){result[@"flush"]=flushRow;if(![flushRow[@"success"] boolValue])failed=true;}
            for(NSDictionary* row in workerRows)if(![row[@"success"] boolValue])failed=true;
        }
        if(!failed && [mode isEqual:@"corrupt"]) {
            // A rejected archive must leave ordinary IR compilation usable.
            uint64_t start=ns();id<MTLRenderPipelineState> p=pipeline(device,descriptors[0],nil,false,&error);
            result[@"fallbackNs"]=@(ns()-start);result[@"fallbackSuccess"]=@(p!=nil);
            if(archive || !p)failed=true;
        }
        result[@"footprintAfterBytes"]=footprint() ?: (id)[NSNull null];result[@"success"]=@(!failed);
        NSData* data=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
        if(![data writeToFile:@(argv[2]) options:NSDataWritingWithoutOverwriting error:nil])return 73;
        printf("mode=%s count=%lu success=%s\n",mode.UTF8String,(unsigned long)count,failed ? "false":"true");
        return failed ? 1:0;
    }
}
