#import <Foundation/Foundation.h>
#include <unistd.h>

// Filesystem/Bundle metadata only: no Metal API, GPU or compilation task.
int main() {
    @autoreleasepool {
        char cache[4096]={}; confstr(_CS_DARWIN_USER_CACHE_DIR,cache,sizeof(cache));
        NSDictionary* result=@{ @"bundleIdentifier":[NSBundle mainBundle].bundleIdentifier ?: @"unbundled",
            @"darwinUserCacheDirectory":@(cache),
            @"foundationCacheDirectories":NSSearchPathForDirectoriesInDomains(NSCachesDirectory,NSUserDomainMask,YES),
            @"homeDirectory":NSHomeDirectory(), @"temporaryDirectory":NSTemporaryDirectory() };
        NSData* data=[NSJSONSerialization dataWithJSONObject:result options:NSJSONWritingPrettyPrinted error:nil];
        puts([[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding].UTF8String);
    }
}
