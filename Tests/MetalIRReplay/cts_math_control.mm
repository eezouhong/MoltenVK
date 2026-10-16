// Diagnostic controls only. Never install this library with the application.
// This forces actual compiler modes while leaving CTS SPIR-V and production
// policy resolution unchanged. All uses must be inside the shared GPU queue.
#include "MVKMetalIRBridge.h"
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <string>

static void fail(const char* text) {fprintf(stderr,"CTS_CONTROL_ERROR %s\n",text);_Exit(95);}
static unsigned mode(const char* name) {
    const char* value=getenv(name);
    if(!value||!value[0]||value[1]||value[0]<'0'||value[0]>'2')fail("invalid explicit mode");
    return unsigned(value[0]-'0');
}
static void* realLibrary() {
    static void* handle=[](){const char* path=getenv("IR_AUDIT_REAL_PLUGIN");
        if(!path||path[0]!='/')fail("real plugin must have an absolute path");
        void* p=dlopen(path,RTLD_NOW|RTLD_LOCAL);if(!p)fail("real plugin load failed");return p;}();
    return handle;
}
template<class T>T symbol(const char* name){auto p=dlsym(realLibrary(),name);if(!p)fail("real plugin symbol missing");return reinterpret_cast<T>(p);}
extern "C" uint32_t MeloNXMetalIRABIVersion(){return symbol<uint32_t(*)()>("MeloNXMetalIRABIVersion")();}
extern "C" const char* MeloNXMetalIRDependencyIdentity(){
    static std::string id=std::string(symbol<const char*(*)()>("MeloNXMetalIRDependencyIdentity")())+
        "+cts-forced-mode="+std::to_string(mode("IR_AUDIT_FORCE_MODE"));return id.c_str();
}
extern "C" int MeloNXCompileMetalIR(const MVKMetalIRCompileRequest* in,MVKMetalIRCompileResult* out){
    if(!in)return 1;auto request=*in;request.mathMode=mode("IR_AUDIT_FORCE_MODE");
    int result=symbol<MVKMetalIRCompileFunction>("MeloNXCompileMetalIR")(&request,out);
    fprintf(stderr,"CTS_IR_MODE requested=%u effective=%u stage=%u status=%d\n",in->mathMode,request.mathMode,request.executionModel,result);
    return result;
}
extern "C" void MeloNXReleaseMetalIR(MVKMetalIRCompileResult* result){symbol<MVKMetalIRReleaseFunction>("MeloNXReleaseMetalIR")(result);}

using Setter=void(*)(id,SEL,MTLMathMode);
static Setter originalSetter;
static void setMode(id object,SEL selector,MTLMathMode requested){
    unsigned target=mode("IR_AUDIT_MSL_MODE");
    const MTLMathMode values[]={MTLMathModeFast,MTLMathModeSafe,MTLMathModeRelaxed};
    originalSetter(object,selector,values[target]);
    if([(MTLCompileOptions*)object mathMode]!=values[target])fail("MSL mode override was not applied");
    fprintf(stderr,"CTS_MSL_MODE requested=%ld effective=%u\n",long(requested),target);
}
__attribute__((constructor))static void configure(){
    if(!getenv("IR_AUDIT_MSL_MODE"))return;
    if(!getenv("MELONX_DEBUG_GRAPHICS_LEASE"))fail("MSL override requires the canonical queue");
    @autoreleasepool {
        MTLCompileOptions* options=[MTLCompileOptions new];
        Method method=class_getInstanceMethod(object_getClass(options),@selector(setMathMode:));
        if(!method)fail("MTLCompileOptions mathMode setter unavailable");
        originalSetter=reinterpret_cast<Setter>(method_setImplementation(method,reinterpret_cast<IMP>(setMode)));
        [options release];
    }
}
