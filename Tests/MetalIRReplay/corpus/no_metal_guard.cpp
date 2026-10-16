// Reject provider loads outside the queue. In explicit queued mode, record them
// instead: loading MSC 3.1.1 also loads Metal/MPS through indirect dependencies.
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
static bool provider(const char* p) {
    return p && (strstr(p, "/Metal.framework/") || strstr(p, "/IOGPU.framework/") ||
                 strstr(p, "/MetalTools.framework/") || strstr(p, "/MetalPerformanceShaders.framework/"));
}
static void refuse(const char* p) {
    if(getenv("B4_QUEUED_COMPILATION")) {
        fprintf(stderr,"B4_QUEUED_METAL_PROVIDER %s\n",p?p:"unknown");
        return;
    }
    fprintf(stderr,"B4_METAL_PROVIDER_BLOCKED %s\n",p?p:"unknown");
    _exit(86);
}
extern "C" int B4NoMetalGuardActive() { return getenv("B4_QUEUED_COMPILATION")?2:1; }
extern "C" void* b4_guard_dlopen(const char* p,int flags) {
    if(provider(p))refuse(p);
    return dlopen(p,flags);
}
struct Interpose { const void* replacement; const void* original; };
__attribute__((used,section("__DATA,__interpose"))) static const Interpose intercept = {
    reinterpret_cast<const void*>(b4_guard_dlopen),reinterpret_cast<const void*>(dlopen)};
static void image_added(const mach_header* header,intptr_t) {
    for(uint32_t i=0;i<_dyld_image_count();++i)
        if(_dyld_get_image_header(i)==header && provider(_dyld_get_image_name(i)))refuse(_dyld_get_image_name(i));
}
__attribute__((constructor)) static void activate() {
    _dyld_register_func_for_add_image(image_added);
    fprintf(stderr,"B4_NO_METAL_GUARD_ACTIVE\n");
}
