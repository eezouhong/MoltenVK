// Bounded opt-in error attribution; this state never owns Metal resources.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>
#include <algorithm>

class MVKGPUFaultDiagnostics {
public:
    static constexpr size_t CommandCapacity = 256;
    static constexpr size_t ErrorCapacity = 4;
    static constexpr size_t EncoderCapacity = 16;
    static constexpr size_t SignpostCapacity = 8;
    static constexpr size_t RecordingCapacity = 16;
    static constexpr size_t EncoderMetadataCapacity = 256;
    static constexpr size_t MaximumFileBytes = 256 * 1024;
    static uint64_t now() {
        static mach_timebase_info_data_t timebase = [] { mach_timebase_info_data_t t; mach_timebase_info(&t); return t; }();
        return (uint64_t)((__uint128_t)mach_absolute_time() * timebase.numer / timebase.denom);
    }
    MVKGPUFaultDiagnostics(const char* directory, uint64_t session) : _directory(directory), _session(session) { _errors = [NSMutableArray new]; }
    ~MVKGPUFaultDiagnostics() { [_errors release]; }
    uint64_t created(id<MTLCommandBuffer> buffer) {
        if (!buffer) return 0;
        std::lock_guard<std::mutex> guard(_mutex);
        uint64_t serial = ++_next;
        _commands[serial % CommandCapacity] = { serial, (uintptr_t)buffer, now(), 0, 0, 0 };
        return serial;
    }
    void commit(id<MTLCommandBuffer> buffer) {
        if (!buffer) return;
        std::lock_guard<std::mutex> guard(_mutex);
        if (Command* c = find((uintptr_t)buffer)) c->submitted = now();
    }
    void attach(id<MTLCommandBuffer> buffer, uint64_t vkRecording, uintptr_t vkCommand) {
        std::lock_guard<std::mutex> guard(_mutex);
        if (Command* c = find((uintptr_t)buffer)) {
            c->recording = vkRecording; c->vkCommand = vkCommand;
            if (c->recordingCount < RecordingCapacity) c->recordings[c->recordingCount++] = vkRecording;
            else c->recordingDrops++;
        }
    }
    void encoder(id<MTLCommandBuffer> buffer, id<MTLCommandEncoder> encoder, const char* kind) {
        if (!encoder) return;
        uint64_t id = 0, ordinal = 0, recording = 0;
        { std::lock_guard<std::mutex> guard(_mutex);
          if (Command* c = find((uintptr_t)buffer)) { id = c->id; ordinal = ++c->encoders; recording = c->recording; } }
        encoder.label = [NSString stringWithFormat:@"mlx:s=%llu:cb=%llu:e=%llu:%s:vkrec=%llu",
            (unsigned long long)_session, (unsigned long long)id, (unsigned long long)ordinal, kind, (unsigned long long)recording];
        std::lock_guard<std::mutex> guard(_mutex);
        Encoder& e = _encoders[++_nextEncoder % EncoderMetadataCapacity];
        e = {}; e.object = (uintptr_t)encoder; e.commandId = id;
        copy(e.label, encoder.label.UTF8String);
    }
    // Fixed first/last binding metadata survives the signpost cap; no resource ownership or heap allocation.
    void pipeline(id<MTLCommandEncoder> encoder, NSString* label) {
        std::lock_guard<std::mutex> guard(_mutex);
        size_t count = std::min<uint64_t>(_nextEncoder, EncoderMetadataCapacity);
        for (size_t i = 0; i < count; i++) {
            Encoder& e = _encoders[(_nextEncoder - i) % EncoderMetadataCapacity];
            if (e.object != (uintptr_t)encoder) continue;
            if (!e.bindings) copy(e.firstPipeline, label.UTF8String);
            copy(e.lastPipeline, label.UTF8String); e.bindings++;
            return;
        }
    }
    void failed(id<MTLCommandBuffer> buffer) {
        if (!buffer || buffer.status != MTLCommandBufferStatusError) return;
        capture(buffer, buffer.error, buffer.GPUStartTime, buffer.GPUEndTime,
            buffer.device.currentAllocatedSize);
    }
    // Separate entry used by the host fault-injection harness (no GPU hang is induced).
    void capture(id<MTLCommandBuffer> buffer, NSError* error, double gpuStart, double gpuEnd, uint64_t allocated) noexcept {
        @try { try { captureImpl(buffer, error, gpuStart, gpuEnd, allocated); } catch (...) { } }
        @catch (NSException*) { }
    }
private:
    void captureImpl(id<MTLCommandBuffer> buffer, NSError* error, double gpuStart, double gpuEnd, uint64_t allocated) {
        if (!error) return;
        std::lock_guard<std::mutex> guard(_mutex);
        Command c = {}; if (Command* found = find((uintptr_t)buffer)) c = *found;
        for (size_t i = 0; i < _errorCount; i++) if (_errorIds[i] == c.id && c.id != 0) { _duplicates++; return; }
        if (_errorCount >= ErrorCapacity) { _errorDrops++; return; }
        _errorIds[_errorCount++] = c.id;
        NSArray<id<MTLCommandBufferEncoderInfo>>* infos = error.userInfo[MTLCommandBufferEncoderInfoErrorKey];
        NSMutableArray* encoders = [NSMutableArray array];
        // Prioritize faulted/affected/pending/unknown before completed encoders.
        const MTLCommandEncoderErrorState priorities[] = { MTLCommandEncoderErrorStateFaulted,
            MTLCommandEncoderErrorStateAffected, MTLCommandEncoderErrorStatePending,
            MTLCommandEncoderErrorStateUnknown, MTLCommandEncoderErrorStateCompleted };
        for (auto wanted : priorities) {
            for (id<MTLCommandBufferEncoderInfo> info in infos) {
                if (encoders.count >= EncoderCapacity || info.errorState != wanted) continue;
                NSMutableArray* signs = [NSMutableArray array];
                NSArray<NSString*>* posts = info.debugSignposts;
                NSUInteger start = posts.count > SignpostCapacity ? posts.count - SignpostCapacity : 0;
                for (NSUInteger i = start; i < posts.count; i++) [signs addObject:bounded(posts[i], 512)];
                const Encoder* binding = nullptr;
                for (const Encoder& e : _encoders) {
                    if (e.commandId == c.id && e.label[0] && [info.label hasPrefix:[NSString stringWithUTF8String:e.label]]) { binding = &e; break; }
                }
                [encoders addObject:@{ @"label":bounded(info.label, 512), @"errorState":stateName(info.errorState),
                    @"debugSignposts":signs, @"signpostsDropped":@(start), @"bindingMetadataAvailable":@(binding != nullptr),
                    @"firstPipeline":binding ? [NSString stringWithUTF8String:binding->firstPipeline] : (id)[NSNull null],
                    @"lastPipeline":binding ? [NSString stringWithUTF8String:binding->lastPipeline] : (id)[NSNull null],
                    @"pipelineBindings":@(binding ? binding->bindings : 0),
                    @"pipelineHistoryIncomplete":@(binding && binding->bindings > 16) }];
            }
        }
        bool valid = std::isfinite(gpuStart) && std::isfinite(gpuEnd) && gpuStart > 0 && gpuEnd >= gpuStart;
        NSMutableArray* recordings = [NSMutableArray array];
        for (size_t i = 0; i < c.recordingCount; i++) [recordings addObject:@(c.recordings[i])];
        NSMutableDictionary* item = [@{ @"metalCommandBufferId":@(c.id), @"vkRecordingId":@(c.recording), @"vkRecordingIds":recordings,
            @"vkRecordingsDropped":@(c.recordingDrops), @"vkCommandBuffer":@(c.vkCommand),
            @"createdMachNanoseconds":@(c.created), @"commitMachNanoseconds":@(c.submitted), @"callbackMachNanoseconds":@(now()),
            @"gpuTimeAvailable":@(valid), @"gpuStartSeconds":valid ? @(gpuStart) : (id)[NSNull null],
            @"gpuEndSeconds":valid ? @(gpuEnd) : (id)[NSNull null], @"gpuDurationSeconds":valid ? @(gpuEnd-gpuStart) : (id)[NSNull null],
            @"errorDomain":bounded(error.domain, 128), @"errorCode":@(error.code), @"errorDescription":bounded(error.localizedDescription, 512),
            @"encoderInformationAvailable":@(infos != nil), @"encoderCount":@(infos.count), @"encodersDropped":@(infos.count-encoders.count),
            @"encoders":encoders, @"metalCurrentAllocatedBytes":@(allocated), @"recordMetadataAvailable":@(c.id != 0) } mutableCopy];
        [_errors addObject:item];
        [item release];
        NSMutableDictionary* snapshot = [[@{ @"schemaVersion":@1, @"pid":@(getpid()), @"gpuSessionId":@(_session),
            @"clock":@"system_mach_time", @"maxErrors":@(ErrorCapacity), @"duplicates":@(_duplicates),
            @"errorsDropped":@(_errorDrops), @"commandMetadataOverwrites":@(_next > CommandCapacity ? _next - CommandCapacity : 0),
            @"encoderMetadataOverwrites":@(_nextEncoder > EncoderMetadataCapacity ? _nextEncoder - EncoderMetadataCapacity : 0),
            @"errors":_errors, @"byteLimitApplied":@NO, @"errorsOmittedByByteLimit":@(_byteDrops) } mutableCopy] autorelease];
        NSData* data = [NSJSONSerialization dataWithJSONObject:snapshot options:0 error:nil];
        // Preserve valid JSON and the newest fault even for pathological driver strings.
        while (data.length > MaximumFileBytes) {
            snapshot[@"byteLimitApplied"] = @YES;
            if (_errors.count > 1) { [_errors removeObjectAtIndex:0]; snapshot[@"errorsOmittedByByteLimit"] = @(++_byteDrops); }
            else if (encoders.count > 0) {
                [encoders removeLastObject]; item[@"encodersDropped"] = @(infos.count - encoders.count);
            } else return;
            data = [NSJSONSerialization dataWithJSONObject:snapshot options:0 error:nil];
        }
        if (!data) return;
        NSString* path = [[NSString stringWithUTF8String:_directory.c_str()] stringByAppendingPathComponent:@"metal-command-errors.json"];
        // Replaces a single bounded artifact on distinct errors; never appends a text log.
        [data writeToFile:path options:NSDataWritingAtomic error:nil];
    }
public:
    static NSString* stateName(MTLCommandEncoderErrorState state) {
        switch (state) {
            case MTLCommandEncoderErrorStateFaulted: return @"faulted";
            case MTLCommandEncoderErrorStateAffected: return @"affected";
            case MTLCommandEncoderErrorStatePending: return @"pending";
            case MTLCommandEncoderErrorStateCompleted: return @"completed";
            default: return @"unknown";
        }
    }
private:
    struct Command {
        uint64_t id; uintptr_t object; uint64_t created, submitted, recording; uintptr_t vkCommand;
        uint64_t encoders = 0, recordingDrops = 0;
        size_t recordingCount = 0; std::array<uint64_t, RecordingCapacity> recordings = {};
    };
    struct Encoder { uintptr_t object = 0; uint64_t commandId = 0, bindings = 0; char label[128] = {}, firstPipeline[512] = {}, lastPipeline[512] = {}; };
    template<size_t N> static void copy(char (&dst)[N], const char* src) {
        if (!src) src = "";
        std::strncpy(dst, src, N - 1); dst[N - 1] = 0;
    }
    Command* find(uintptr_t object) {
        size_t count = _next < CommandCapacity ? _next : CommandCapacity;
        for (size_t i = 0; i < count; i++) { Command& c = _commands[(_next - i) % CommandCapacity]; if (c.object == object) return &c; }
        return nullptr;
    }
    static NSString* bounded(NSString* text, NSUInteger limit) {
        if (!text) return @"";
        return text.length <= limit ? text : [text substringToIndex:limit];
    }
    std::mutex _mutex;
    std::string _directory;
    uint64_t _session, _next = 0, _errorDrops = 0, _duplicates = 0, _byteDrops = 0;
    size_t _errorCount = 0;
    std::array<uint64_t, ErrorCapacity> _errorIds = {};
    std::array<Command, CommandCapacity> _commands = {};
    uint64_t _nextEncoder = 0;
    std::array<Encoder, EncoderMetadataCapacity> _encoders = {};
    NSMutableArray* _errors;
};
