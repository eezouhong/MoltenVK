// Fault attribution test: synthetic NSError encoder reports, no GPU hang.
#import "MVKGPUFaultDiagnostics.h"
#include <cassert>
#include <cstdio>
@interface EncoderInfo : NSObject <MTLCommandBufferEncoderInfo>
@property(nonatomic, retain) NSString* label;
@property(nonatomic, retain) NSArray<NSString*>* debugSignposts;
@property(nonatomic) MTLCommandEncoderErrorState errorState;
@end
@implementation EncoderInfo
- (void)dealloc { [_label release]; [_debugSignposts release]; [super dealloc]; }
@end
static NSDictionary* read(NSString* dir) {
    NSData* data = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"metal-command-errors.json"]];
    assert(data && data.length <= MVKGPUFaultDiagnostics::MaximumFileBytes);
    return [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
}
int main(int argc, char** argv) { @autoreleasepool {
    assert(argc == 2);
    NSString* dir = [NSString stringWithUTF8String:argv[1]];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    id<MTLDevice> device = MTLCreateSystemDefaultDevice(); assert(device);
    id<MTLCommandQueue> queue = [device newCommandQueue]; assert(queue);
    MVKGPUFaultDiagnostics diagnostics(argv[1], 3);
    MTLCommandBufferDescriptor* descriptor = [MTLCommandBufferDescriptor new];
    descriptor.retainedReferences = NO;
    assert(descriptor.errorOptions == MTLCommandBufferErrorOptionNone);
    descriptor.errorOptions = MTLCommandBufferErrorOptionEncoderExecutionStatus;
    id<MTLCommandBuffer> buffer = [queue commandBufferWithDescriptor:descriptor]; assert(!buffer.retainedReferences);
    uint64_t serial = diagnostics.created(buffer);
    diagnostics.attach(buffer, 10, 100); diagnostics.attach(buffer, 11, 101);
    id<MTLBlitCommandEncoder> encoder = [buffer blitCommandEncoder];
    diagnostics.encoder(buffer, encoder, "blit:test");
    for (int i = 0; i < 40; i++) diagnostics.pipeline(encoder, [NSString stringWithFormat:@"shader-%d", i]);
    NSString* label = [[encoder.label copy] autorelease];
    [encoder endEncoding]; diagnostics.commit(buffer); [buffer commit]; [buffer waitUntilCompleted];
    NSMutableArray* infos = [NSMutableArray array];
    for (int i = 0; i < 30; i++) {
        EncoderInfo* info = [[[EncoderInfo alloc] init] autorelease]; info.label = @"completed";
        info.errorState = MTLCommandEncoderErrorStateCompleted; info.debugSignposts = @[]; [infos addObject:info];
    }
    EncoderInfo* fault = [[[EncoderInfo alloc] init] autorelease]; fault.label = label;
    fault.errorState = MTLCommandEncoderErrorStateFaulted;
    NSMutableArray* posts = [NSMutableArray array];
    for (int i = 0; i < 40; i++) [posts addObject:[NSString stringWithFormat:@"shader-%d", i]];
    fault.debugSignposts = posts; [infos addObject:fault];
    NSError* error = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorTimeout
        userInfo:@{MTLCommandBufferEncoderInfoErrorKey:infos}];
    diagnostics.capture(buffer, error, 0, 0, 123);
    NSDictionary* snapshot = read(dir); NSDictionary* item = snapshot[@"errors"][0]; NSDictionary* first = item[@"encoders"][0];
    assert([snapshot[@"gpuSessionId"] unsignedIntValue] == 3);
    assert([item[@"metalCommandBufferId"] unsignedLongLongValue] == serial);
    assert(([item[@"vkRecordingIds"] isEqual:@[@10,@11]]));
    assert(![item[@"gpuTimeAvailable"] boolValue] && item[@"gpuStartSeconds"] == [NSNull null]);
    assert([first[@"errorState"] isEqual:@"faulted"] && [first[@"lastPipeline"] isEqual:@"shader-39"]);
    assert([first[@"firstPipeline"] isEqual:@"shader-0"] && [first[@"pipelineHistoryIncomplete"] boolValue]);
    assert([first[@"debugSignposts"] count] == 8 && [item[@"encodersDropped"] unsignedIntValue] == 15);
    diagnostics.capture(buffer, error, 1, 2, 123); assert([read(dir)[@"errors"] count] == 1);
    // Saturate strings and error count: bounded valid JSON, prioritizing the newest fault.
    NSString* huge = [@"\"" stringByPaddingToLength:8000 withString:@"\"" startingAtIndex:0];
    fault.debugSignposts = @[huge,huge,huge,huge,huge,huge,huge,huge]; fault.label = huge;
    for (int i = 0; i < 6; i++) {
        id<MTLCommandBuffer> next = [queue commandBufferWithDescriptor:descriptor]; diagnostics.created(next);
        diagnostics.capture(next, error, 1, 2, 0);
    }
    snapshot = read(dir); assert([snapshot[@"errors"] count] <= 4);
    printf("PASS: native fault attribution, first/last pipeline, multiple recordings, missing times, dedup, bounds; fixed state=%zu bytes\n", sizeof(diagnostics));
    [descriptor release]; [queue release]; [device release];
} }
