#include "MVKReplayTrace.h"
#include "MVKMetalIR.h"
#include "MVKMetalIRCache.h"
#include "MVKMetalIRResidentCache.h"
#include "MVKMetalIRCompilerLimit.h"
#include "MVKPipeline.h"
#include "MVKShaderModule.h"
#include "MVKShaderMathPolicy.h"
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <malloc/malloc.h>
#include "MVKSPIRVMathFlags.h"
#include "mvkGitRevDerived.h"

bool mvkMetalIREnabled() {
    const char* value = getenv("MELONX_EXPERIMENTAL_METAL_IR");
    return value && strcmp(value, "1") == 0;
}
MVKMetalIRArtifact::~MVKMetalIRArtifact(){[function release];[library release];}

namespace {
mvkir::CompilerLimit& compilerLimit() {
    static mvkir::CompilerLimit limit([] {
        const char* value=getenv("MELONX_METAL_IR_COMPILE_WORKERS");
        if (!value || !*value) return 2u;
        char* end=nullptr;
        unsigned long count=strtoul(value,&end,10);
        return end && !*end && count>=1 && count<=8 ? unsigned(count) : 2u;
    }());
    return limit;
}
struct Plugin {
    void* library=nullptr;MVKMetalIRCompileFunction compile=nullptr;MVKMetalIRReleaseFunction release=nullptr;
    std::string identity;
};
Plugin& plugin() {
    static Plugin result=[] {
        Plugin p;
        fprintf(stderr,"MetalIR native base MoltenVK %s %s (local experimental changes)\n",MVK_VERSION_STRING,mvkRevString);
        const char* path=getenv("MELONX_METAL_IR_PLUGIN");if(!path||!*path)return p;
        p.library=dlopen(path,RTLD_NOW|RTLD_LOCAL);
        if(!p.library){fprintf(stderr,"MetalIR compiler unavailable: %s\n",dlerror());return p;}
        p.compile=(MVKMetalIRCompileFunction)dlsym(p.library,"MeloNXCompileMetalIR");
        p.release=(MVKMetalIRReleaseFunction)dlsym(p.library,"MeloNXReleaseMetalIR");
        auto abiVersion=(uint32_t(*)())dlsym(p.library,"MeloNXMetalIRABIVersion");
        auto dependencyIdentity=(const char*(*)())dlsym(p.library,"MeloNXMetalIRDependencyIdentity");
        if(!p.compile||!p.release||!abiVersion||abiVersion()!=MVK_METAL_IR_ABI_VERSION||!dependencyIdentity) {
            fprintf(stderr,"MetalIR compiler ABI mismatch; IR unavailable\n");
            dlclose(p.library);return Plugin{};
        }
        std::ifstream file(path,std::ios::binary);std::string bytes((std::istreambuf_iterator<char>(file)),{});
        if(bytes.empty()){dlclose(p.library);return Plugin{};}
        unsigned char hash[CC_SHA256_DIGEST_LENGTH];CC_SHA256(bytes.data(),(CC_LONG)bytes.size(),hash);
        char hex[65];for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",hash[i]);p.identity=hex;
        p.identity+=dependencyIdentity();
        for(const char* symbol:{"spirv_to_dxil", "IRCompilerCreate"}) {
            Dl_info dependency{};
            void* address=dlsym(p.library,symbol);
            // iOS links Mesa into the four-export compiler framework. Its
            // bytes are already covered by the plugin hash, with no sidecar.
            if(!address && !strcmp(symbol,"spirv_to_dxil")) {
                p.identity+="embedded-mesa";continue;
            }
            if(!address || !dladdr(address,&dependency) || !dependency.dli_fname) {
                fprintf(stderr,"MetalIR dependency identity unavailable: %s\n",symbol);
                dlclose(p.library);return Plugin{};
            }
            std::ifstream sidecar(dependency.dli_fname,std::ios::binary);
            CC_SHA256_CTX sidecarHash;CC_SHA256_Init(&sidecarHash);
            char chunk[64*1024];uint64_t size=0;
            while(sidecar.read(chunk,sizeof(chunk)) || sidecar.gcount()) {
                auto count=sidecar.gcount();size+=count;CC_SHA256_Update(&sidecarHash,chunk,CC_LONG(count));
            }
            if(!size || sidecar.bad()){dlclose(p.library);return Plugin{};}
            CC_SHA256_Final(hash,&sidecarHash);
            for(int i=0;i<32;++i)snprintf(hex+i*2,3,"%02x",hash[i]);
            p.identity+=hex;
        }
        fprintf(stderr,"MetalIR experimental compiler loaded; ABI %u\n",MVK_METAL_IR_ABI_VERSION);
        return p;
    }();return result;
}
std::atomic<bool>& telemetryStorage() {
    static std::atomic<bool> enabled{[] {const char* v=getenv("MELONX_METAL_IR_TELEMETRY");return v&&strcmp(v,"1")==0;}()};
    return enabled;
}
bool telemetryEnabled() { return telemetryStorage().load(std::memory_order_relaxed); }
using IRClock=std::chrono::steady_clock;
static uint64_t elapsedNs(IRClock::time_point start) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(IRClock::now()-start).count();
}
struct DeviceArtifacts {
    using Cache=mvkir::ResidentCache<MVKMetalIRArtifact>;
    Cache cache;
    std::shared_ptr<mvkir::DiskCache> disk;
    std::string diskDirectory;
    std::mutex diskConfigurationLock;
    std::atomic<uint64_t> compilerCalls{0},diskHits{0},mesaNs{0},converterNs{0},rasterAdapterNs{0},libraryNs{0},reflectionNs{0},rejected{0},psoNs{0};
    DeviceArtifacts():cache(retainedCount(),8*1024*1024) {
        const char* directory=getenv("MELONX_METAL_IR_CACHE");
        if(directory&&*directory) {
            diskDirectory=directory;
            disk=std::make_shared<mvkir::DiskCache>(diskDirectory);
        }
    }
    static size_t retainedCount() {
        const char* v=getenv("MELONX_METAL_IR_MEMORY_CACHE");
        return v&&strcmp(v,"0")==0?0:64;
    }
};
std::mutex devicesLock;
std::unordered_map<MVKDevice*,std::shared_ptr<DeviceArtifacts>> devices;
std::shared_ptr<DeviceArtifacts> deviceArtifacts(MVKDevice* device) {
    std::lock_guard<std::mutex> lock(devicesLock);
    auto& slot=devices[device];
    if(!slot)slot=std::make_shared<DeviceArtifacts>();
    return slot;
}
uint32_t shaderMathMode(const std::vector<uint32_t>& code, const char* entry,
                        spv::ExecutionModel execution, MVKConfigFastMath preference) {
    switch (mvkshader::resolveMathMode(preference, mvkshader::spirvMathFlags(code,entry,execution))) {
        case mvkshader::MathMode::Safe: return MVK_METAL_IR_MATH_SAFE;
        case mvkshader::MathMode::Relaxed: return MVK_METAL_IR_MATH_RELAXED;
        case mvkshader::MathMode::Fast: return MVK_METAL_IR_MATH_FAST;
    }
    return MVK_METAL_IR_MATH_SAFE;
}

std::string keyFor(const MVKMetalIRCompileRequest& request) {
    CC_SHA256_CTX hash;CC_SHA256_Init(&hash);
    auto add=[&](const void* data,size_t size){CC_SHA256_Update(&hash,data,(CC_LONG)size);};
    const uint32_t cacheVersion=5;add(&cacheVersion,4);
    auto sized=[&](const void* data,size_t bytes){uint64_t length=bytes;add(&length,sizeof(length));if(bytes)add(data,bytes);};
    sized(mvkRevString,strlen(mvkRevString));
    sized(plugin().identity.data(),plugin().identity.size());add(&request.abiVersion,4);add(&request.executionModel,4);
    sized(request.words,request.wordCount*4);sized(request.entry,strlen(request.entry));
    sized(request.bindings,request.bindingCount*sizeof(MVKMetalIRBinding));add(&request.setCount,sizeof(request.setCount));
    add(&request.pushConstantSize,4);add(&request.preserveInvariance,4);add(&request.mathMode,4);
    add(&request.vertexTransformFlags,4);
    add(&request.runtimeOptions,4);
    unsigned char digest[32];CC_SHA256_Final(digest,&hash);char text[65];for(int i=0;i<32;++i)snprintf(text+i*2,3,"%02x",digest[i]);return text;
}
}

bool mvkMetalIRCompilerAvailable() {
    return plugin().compile && plugin().release;
}

VkResult mvkMetalIRConfigureCache(MVKDevice* device,const char* directory,uint64_t maxBytes) {
    if(!device||!device->isMetalIRShaderCompilerEnabled()||!directory||!*directory)return VK_ERROR_INITIALIZATION_FAILED;
    try {
        auto state=deviceArtifacts(device);
        std::lock_guard<std::mutex> lock(state->diskConfigurationLock);
        if(state->diskDirectory==directory&&state->disk&&state->disk->enabled())return VK_SUCCESS;
        // Internal helpers can compile before the first title program supplies
        // its cache path. Allow that initial configuration, but never redirect
        // an already configured device to another title's directory.
        if(!state->diskDirectory.empty())return VK_ERROR_INITIALIZATION_FAILED;
        auto disk=std::make_shared<mvkir::DiskCache>(directory,maxBytes);
        if(!disk->enabled())return VK_ERROR_INITIALIZATION_FAILED;
        state->diskDirectory=directory;
        std::atomic_store(&state->disk,std::move(disk));
        return VK_SUCCESS;
    } catch(...) {return VK_ERROR_OUT_OF_HOST_MEMORY;}
}

uint64_t mvkMetalIRRelieveCompilerMemory() {
    uint64_t released=0;
    compilerLimit().relieveWhenIdle(std::chrono::milliseconds(500),[&] {
        released=malloc_zone_pressure_relief(nullptr,0);
    });
    return released;
}

uint32_t mvkMetalIRCompilerAdmissionStatistics(uint64_t* output,uint32_t capacity) {
    if (!output || capacity<6) return 0;
    auto value=compilerLimit().stats();
    output[0]=value.active;output[1]=value.waiting;output[2]=value.peak;output[3]=value.limit;
    output[4]=value.batches;output[5]=value.reliefs;
    return 6;
}

uint32_t mvkMetalIRSetProbeDiagnostics(uint32_t flags) {
    // The probe runs outside a game. Return a token for exact restoration;
    // never leave detailed timers enabled for subsequent game sessions.
    if ((flags & ~7u) || (flags & 6u) == 6u) return UINT32_MAX;
    const auto nextMode = flags & 2u ? mvkreplay::Mode::Detailed :
                          flags & 4u ? mvkreplay::Mode::Coarse : mvkreplay::Mode::Off;
    const auto oldMode = mvkreplay::setMode(nextMode);
    const bool oldTelemetry = telemetryStorage().exchange(flags & 1u,std::memory_order_relaxed);
    return (oldTelemetry ? 1u : 0u) |
           (oldMode == mvkreplay::Mode::Detailed ? 2u : oldMode == mvkreplay::Mode::Coarse ? 4u : 0u);
}

uint32_t mvkMetalIRCompilerStatistics(MVKDevice* device, uint64_t* output, uint32_t capacity) {
    if (!device || !output || capacity < 7 || !telemetryEnabled()) return 0;
    const uint32_t written=capacity>=9?9:capacity>=8?8:7;
    std::fill(output, output + written, 0);
    std::shared_ptr<DeviceArtifacts> state;
    {
        std::lock_guard<std::mutex> lock(devicesLock);
        auto found = devices.find(device);
        if (found == devices.end()) return written;
        state = found->second;
    }
    output[0] = state->compilerCalls.load(std::memory_order_relaxed);
    output[1] = state->diskHits.load(std::memory_order_relaxed);
    output[2] = state->mesaNs.load(std::memory_order_relaxed);
    output[3] = state->converterNs.load(std::memory_order_relaxed);
    output[4] = state->rasterAdapterNs.load(std::memory_order_relaxed);
    output[5] = state->libraryNs.load(std::memory_order_relaxed);
    output[6] = state->reflectionNs.load(std::memory_order_relaxed);
    if(written>=8)output[7]=state->rejected.load(std::memory_order_relaxed);
    if(written>=9)output[8]=state->psoNs.load(std::memory_order_relaxed);
    return written;
}

MVKMetalIRPSOTimer::MVKMetalIRPSOTimer(MVKDevice* device) {
    if(device&&device->isMetalIRShaderCompilerEnabled()&&telemetryEnabled()) {
        _device=device;
        _start=std::chrono::duration_cast<std::chrono::nanoseconds>(IRClock::now().time_since_epoch()).count();
    }
}
MVKMetalIRPSOTimer::~MVKMetalIRPSOTimer() {
    if(!_device)return;
    const uint64_t now=std::chrono::duration_cast<std::chrono::nanoseconds>(IRClock::now().time_since_epoch()).count();
    // The pipeline owns its device until construction/compilation completes.
    try {deviceArtifacts(_device)->psoNs.fetch_add(now-_start,std::memory_order_relaxed);}
    catch(...) {} // Diagnostics must not turn a successful compile into failure.
}

void mvkMetalIRDestroyDevice(MVKDevice* device) {
    std::shared_ptr<DeviceArtifacts> state;
    {
        std::lock_guard<std::mutex> lock(devicesLock);
        auto found=devices.find(device);
        if(found==devices.end())return;
        state=std::move(found->second);devices.erase(found);
    }
    if(telemetryEnabled()) {
        auto s=state->cache.stats();
        fprintf(stderr,"MetalIR cache summary: hits=%llu misses=%llu waits=%llu evictions=%llu retained=%zu code_bytes=%zu compiler_calls=%llu disk_hits=%llu mesa_ms=%.3f converter_ms=%.3f raster_adapter_ms=%.3f library_ms=%.3f reflection_ms=%.3f\n",
            (unsigned long long)s.hits,(unsigned long long)s.misses,(unsigned long long)s.waits,(unsigned long long)s.evictions,s.retained,s.retainedBytes,
            (unsigned long long)state->compilerCalls.load(),(unsigned long long)state->diskHits.load(),state->mesaNs.load()/1e6,
            state->converterNs.load()/1e6,state->rasterAdapterNs.load()/1e6,state->libraryNs.load()/1e6,state->reflectionNs.load()/1e6);
    }
    if(telemetryEnabled())if(auto disk=std::atomic_load(&state->disk)) {
        disk->waitIdle();
        fprintf(stderr,"MetalIR disk summary: dropped_writes=%llu failed_writes=%llu\n",
            (unsigned long long)disk->droppedWrites(),(unsigned long long)disk->failedWrites());
    }
    // Release cached Metal objects outside the global map lock. Vulkan requires
    // users to finish device calls before destruction, so pointer reuse is safe.
}

static std::shared_ptr<MVKMetalIRArtifact> compileMetalIR(MVKPipeline* owner,MVKPipelineLayout* layout,
                                                    MVKShaderModule* module,const VkPipelineShaderStageCreateInfo* stage,uint32_t vertexTransformFlags,uint32_t runtimeOptions) {
    auto reject = [&](const char* reason, VkResult result = VK_ERROR_FEATURE_NOT_PRESENT) -> std::shared_ptr<MVKMetalIRArtifact> {
        if(telemetryEnabled()&&result!=VK_PIPELINE_COMPILE_REQUIRED) {
            try {++deviceArtifacts(owner->getDevice())->rejected;} catch(...) {}
        }
        owner->setConfigurationResult(owner->reportError(result, "MetalIR shader rejected: %s; MSL fallback disabled.", reason));
        return {};
    };
    if (!module || !stage) return reject("missing shader stage");
    if (owner->shouldFailOnPipelineCompileRequired()) return reject("pipeline compilation required", VK_PIPELINE_COMPILE_REQUIRED);
    if (!plugin().compile || !plugin().release) return reject("compiler plugin unavailable or ABI incompatible", VK_ERROR_INITIALIZATION_FAILED);
    if(stage->pSpecializationInfo&&stage->pSpecializationInfo->mapEntryCount)return reject("specialization constants unsupported");
    if(stage->stage!=VK_SHADER_STAGE_VERTEX_BIT&&stage->stage!=VK_SHADER_STAGE_FRAGMENT_BIT&&stage->stage!=VK_SHADER_STAGE_COMPUTE_BIT)return reject("shader stage unsupported");
    const char* stages=getenv("MELONX_METAL_IR_STAGES");
    if(stages&&((strcmp(stages,"graphics")==0&&stage->stage==VK_SHADER_STAGE_COMPUTE_BIT)||
                (strcmp(stages,"compute")==0&&stage->stage!=VK_SHADER_STAGE_COMPUTE_BIT)))return reject("stage filter excludes the requested IR shader");
    if(layout->getDescriptorSetCount()>8)return reject("descriptor set count exceeds IR ABI");
    std::vector<MVKMetalIRBinding> bindings;std::vector<uint32_t> sizes;
    for(uint32_t set=0;set<layout->getDescriptorSetCount();++set) {
        auto* dsl=layout->getDescriptorSetLayout(set);
        uint32_t count = mvkMetalIRDescriptorCount(dsl);
        sizes.push_back(count);
        // Vulkan permits empty reserved sets. MoltenVK correctly gives them no
        // argument buffer; they still occupy a stable slot in the root-table ABI.
        if (!count) continue;
        if(dsl->argBufMode()!=MVKArgumentBufferMode::Metal3) {
            std::string reason = "descriptor set " + std::to_string(set) +
                " requires argument-buffer mode " + std::to_string(static_cast<uint32_t>(dsl->argBufMode()));
            return reject(reason.c_str());
        }
        if(dsl->isCPUAllocationVariable()||dsl->isGPUAllocationVariable())return reject("variable descriptor allocation unsupported");
        if(dsl->dynamicOffsetCount(0))return reject("dynamic descriptor offsets unsupported");
        if(!mvkMetalIRTableBytes(dsl))return reject("descriptor layout lacks an IR table");
        for(const auto& b:dsl->bindings()) {
            if(b.descriptorType==VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK||b.descriptorType==VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR)return reject("descriptor type unsupported");
            if(b.gpuLayout==MVKDescriptorGPULayout::Tex2SampSoA||b.gpuLayout==MVKDescriptorGPULayout::Tex3SampSoA)return reject("multi-plane descriptor unsupported");
            MVKMetalIRBinding irBinding{set,b.binding,b.descriptorCount,(uint32_t)b.descriptorType,mvkMetalIRDenseBinding(dsl,b.binding),{}};
            std::copy(std::begin(b.metalIRTableOffsets),std::end(b.metalIRTableOffsets),irBinding.tableOffsets);
            bindings.push_back(irBinding);
        }
    }
    const auto& code=module->getSPIRV();
    for(size_t pos=5;pos<code.size();) {
        uint32_t count=code[pos]>>16,op=code[pos]&65535;
        if(!count||pos+count>code.size())return reject("malformed SPIR-V instruction");
        if(op==spv::OpCapability&&count>=2&&
           (code[pos+1]==spv::CapabilityPhysicalStorageBufferAddresses||code[pos+1]==spv::CapabilityTransformFeedback))return reject("physical addresses or transform feedback unsupported");
        if(op==spv::OpExecutionMode&&count>=3&&
           (code[pos+2]==spv::ExecutionModeDenormPreserve||code[pos+2]==spv::ExecutionModeRoundingModeRTZ))return reject("floating-point execution mode unsupported");
        pos+=count;
    }
    uint32_t execution=stage->stage==VK_SHADER_STAGE_VERTEX_BIT?0:stage->stage==VK_SHADER_STAGE_FRAGMENT_BIT?4:5;
    const char* strictMathOption=getenv("MELONX_METAL_IR_STRICT_MATH");
    uint32_t mathMode=strictMathOption?(strcmp(strictMathOption,"1")==0):shaderMathMode(code,stage->pName,(spv::ExecutionModel)execution,owner->getMVKConfig().fastMathEnabled);
    SPIRV_CROSS_NAMESPACE::Compiler reflect(code);
    reflect.set_entry_point(stage->pName,(spv::ExecutionModel)execution);
    auto active=reflect.get_shader_resources(reflect.get_active_interface_variables());
    mvkir::DiskCache::Reflection shaderReflection;
    shaderReflection.usesPushConstants=!active.push_constant_buffers.empty();
    for(const auto& input:active.builtin_inputs)
        shaderReflection.usesPointCoordinates |= input.builtin==spv::BuiltInPointCoord;
    auto addResources=[&](const auto& resources){for(const auto& resource:resources){
        uint32_t set=reflect.get_decoration(resource.id,spv::DecorationDescriptorSet);
        uint32_t binding=reflect.get_decoration(resource.id,spv::DecorationBinding);
        if(set>=sizes.size())throw std::runtime_error("active descriptor set absent from layout");
        shaderReflection.usedSets|=1ull<<set;
        shaderReflection.usedBindings.push_back(((uint64_t)set<<32)|binding);
    }};
    addResources(active.uniform_buffers);addResources(active.storage_buffers);
    addResources(active.sampled_images);addResources(active.separate_images);addResources(active.separate_samplers);
    addResources(active.storage_images);addResources(active.subpass_inputs);
    std::sort(shaderReflection.usedBindings.begin(),shaderReflection.usedBindings.end());
    shaderReflection.usedBindings.erase(std::unique(shaderReflection.usedBindings.begin(),shaderReflection.usedBindings.end()),shaderReflection.usedBindings.end());
    for(uint64_t used:shaderReflection.usedBindings)
        if(std::none_of(bindings.begin(),bindings.end(),[&](const auto& b){return used==((uint64_t(b.set)<<32)|b.binding);}))
            return reject("active descriptor binding absent from layout");
    bindings.erase(std::remove_if(bindings.begin(),bindings.end(),[&](const auto& b){
        return !std::binary_search(shaderReflection.usedBindings.begin(),shaderReflection.usedBindings.end(),(uint64_t(b.set)<<32)|b.binding);
    }),bindings.end());
    // Preserve each used binding's physical offsets and dense register number.
    // Only trailing unused root slots can disappear without changing the ABI.
    while(!sizes.empty() && !(shaderReflection.usedSets & (1ull<<(sizes.size()-1))))sizes.pop_back();
    MVKMetalIRCompileRequest request={MVK_METAL_IR_ABI_VERSION,execution,code.data(),code.size(),stage->pName,bindings.data(),bindings.size(),sizes.data(),(uint32_t)sizes.size(),layout->getPushConstantsLength(),execution==0,mathMode,vertexTransformFlags,runtimeOptions};
    std::string key=keyFor(request);
    auto state=deviceArtifacts(owner->getDevice());
    auto compiled = state->cache.get(key,[&]() -> DeviceArtifacts::Cache::Result {
    mvkir::CompilerLimit::Permit admission(compilerLimit());
    std::shared_ptr<MVKMetalIRArtifact> artifact;
    MVKMetalIRCompileResult result={};
    struct LibraryBytes {
        dispatch_data_t data=nullptr;
        ~LibraryBytes(){if(data)dispatch_release(data);}
        void reset(){if(data)dispatch_release(data);data=nullptr;}
    } libraryBytes;
    auto disk=std::atomic_load(&state->disk);
    mvkir::DiskCache::Reflection cachedReflection;
    bool fromDisk=disk&&disk->load(key,execution,result,cachedReflection);
    bool telemetry=telemetryEnabled();
    if(telemetry&&fromDisk)++state->diskHits;
    try {
      for(int attempt=0;attempt<2;++attempt) {
        if(telemetry&&!fromDisk)++state->compilerCalls;
        int compileStatus=fromDisk?0:plugin().compile(&request,&result);
        if(telemetry&&!fromDisk) {
            state->mesaNs+=(uint64_t)(std::max(0.0,result.mesaMs)*1e6);
            state->converterNs+=(uint64_t)(std::max(0.0,result.converterMs)*1e6);
            state->rasterAdapterNs+=(uint64_t)(std::max(0.0,result.rasterAdapterMs)*1e6);
        }
        bool validResult=result.abiVersion==MVK_METAL_IR_ABI_VERSION&&result.entry[0]&&
            memchr(result.entry,0,sizeof(result.entry))&&result.metallib&&result.metallibSize&&result.metallibSize<=64*1024*1024;
        validResult&=!(result.runtimeFlags&~(MVK_METAL_IR_RUNTIME_DATA|MVK_METAL_IR_DRAW_PARAMETERS|MVK_METAL_IR_UNIT_POINT_SIZE|MVK_METAL_IR_NATIVE_POINT_SIZE|MVK_METAL_IR_NATIVE_POINT_COORDINATES|MVK_METAL_IR_DRAW_BASES|MVK_METAL_IR_DISPATCH_GROUPS));
        validResult&=execution==0||!(result.runtimeFlags&(MVK_METAL_IR_DRAW_PARAMETERS|MVK_METAL_IR_UNIT_POINT_SIZE|MVK_METAL_IR_NATIVE_POINT_SIZE));
        validResult&=execution==4||!(result.runtimeFlags&MVK_METAL_IR_NATIVE_POINT_COORDINATES);
        validResult&=execution==0||!(result.runtimeFlags&MVK_METAL_IR_DRAW_BASES);
        validResult&=execution==5||!(result.runtimeFlags&MVK_METAL_IR_DISPATCH_GROUPS);
        validResult&=execution!=4||!(result.runtimeFlags&MVK_METAL_IR_RUNTIME_DATA);
        if(execution==5)validResult&=result.threadgroupSize[0]&&result.threadgroupSize[0]<=1024&&
            result.threadgroupSize[1]&&result.threadgroupSize[1]<=1024&&result.threadgroupSize[2]&&result.threadgroupSize[2]<=1024&&
            (uint64_t)result.threadgroupSize[0]*result.threadgroupSize[1]*result.threadgroupSize[2]<=1024;
        if(compileStatus==0&&validResult) {
            artifact=std::make_shared<MVKMetalIRArtifact>();
            NSError* error=nil;
            auto libraryStart=telemetry?IRClock::now():IRClock::time_point{};
            libraryBytes.reset();
            libraryBytes.data=dispatch_data_create(result.metallib,result.metallibSize,nullptr,DISPATCH_DATA_DESTRUCTOR_FREE);
            if(!libraryBytes.data)throw std::bad_alloc();
            result.metallib=nullptr; // dispatch_data owns this malloc allocation.
            { mvkreplay::Timer trace(mvkreplay::IRLibrary); artifact->library=[owner->getMTLDevice() newLibraryWithData:libraryBytes.data error:&error]; }
            if(artifact->library) { mvkreplay::Timer trace(mvkreplay::IRFunction); artifact->function=[artifact->library newFunctionWithName:@(result.entry)]; }
            if(telemetry)state->libraryNs+=elapsedNs(libraryStart);
            MTLFunctionType expectedType=execution==0?MTLFunctionTypeVertex:execution==4?MTLFunctionTypeFragment:MTLFunctionTypeKernel;
            if(artifact->function&&artifact->function.functionType!=expectedType){[artifact->function release];artifact->function=nil;}
            if(!artifact->function){artifact.reset();owner->reportMessage(MVK_CONFIG_LOG_LEVEL_INFO,"MetalIR library rejected: %s",error.localizedDescription.UTF8String?:"missing entry");}
            else {
                artifact->setCount=request.setCount;artifact->pushConstantSize=request.pushConstantSize;
                artifact->runtimeFlags=result.runtimeFlags;
                memcpy(artifact->vertexAttributes,result.vertexAttributes,sizeof(artifact->vertexAttributes));
                for(int i=0;i<3;++i)artifact->threadgroupSize[i]=result.threadgroupSize[i]?result.threadgroupSize[i]:1;
                // Vulkan permits unused descriptor sets to remain unbound or
                // invalidated. Only touch sets statically used by this entry.
                auto reflectionStart=telemetry?IRClock::now():IRClock::time_point{};
                mvkreplay::Timer reflectionTrace(mvkreplay::IRReflection);
                if(fromDisk) {
                    artifact->usedSets=cachedReflection.usedSets;
                    artifact->usedBindings=cachedReflection.usedBindings;
                    artifact->vertexLocations=cachedReflection.vertexLocations;
                    artifact->usesPushConstants=cachedReflection.usesPushConstants;
                    artifact->usesPointCoordinates=cachedReflection.usesPointCoordinates;
                } else {
                artifact->usedSets=shaderReflection.usedSets;
                artifact->usedBindings=shaderReflection.usedBindings;
                artifact->usesPushConstants=shaderReflection.usesPushConstants;
                artifact->usesPointCoordinates=shaderReflection.usesPointCoordinates;
                if(execution==0) {
                    MVKSmallVector<mvk::SPIRVShaderInterfaceVariable,16> inputs;std::string errorLog;
                    if(!mvk::getShaderInputs(code,spv::ExecutionModelVertex,stage->pName,inputs,errorLog))artifact.reset();
                    else for(const auto& input:inputs)if(input.isUsed&&input.builtin==spv::BuiltInMax){
                        if(input.location>=32||artifact->vertexAttributes[input.location]>=32){artifact.reset();break;}
                        artifact->vertexLocations|=1ull<<input.location;
                    }
                }
                }
                if(telemetry)state->reflectionNs+=elapsedNs(reflectionStart);
                if(artifact)owner->reportMessage(MVK_CONFIG_LOG_LEVEL_DEBUG,"MetalIR %s stage %u: Mesa %.3f ms, converter %.3f ms, raster adapter %.3f ms, active sets 0x%llx, math mode %u, push bytes %u/%u, runtime flags 0x%x",fromDisk?"restored":"compiled",execution,result.mesaMs,result.converterMs,result.rasterAdapterMs,(unsigned long long)artifact->usedSets,request.mathMode,artifact->usesPushConstants?artifact->pushConstantSize:0u,artifact->pushConstantSize,artifact->runtimeFlags);
            }
        }
        if(!artifact&&fromDisk){
            disk->invalidate(key);free(result.metallib);result={};fromDisk=false;continue;
        }
        if(artifact&&!fromDisk&&disk) {
            mvkir::DiskCache::Reflection reflection;
            reflection.usedSets=artifact->usedSets;reflection.usedBindings=artifact->usedBindings;
            reflection.vertexLocations=artifact->vertexLocations;reflection.usesPushConstants=artifact->usesPushConstants;
            reflection.usesPointCoordinates=artifact->usesPointCoordinates;
            auto borrowed=result;
            // Keep a borrowed contiguous view alive only for the queue's one
            // persistence copy; library construction itself performs no copy.
            const void* storage=nullptr;size_t size=0;
            dispatch_data_t mapped=dispatch_data_create_map(libraryBytes.data,&storage,&size);
            if(mapped) {
                borrowed.metallib=const_cast<void*>(storage);borrowed.metallibSize=size;
                disk->store(key,execution,borrowed,reflection);
                dispatch_release(mapped);
            }
        }
        break;
      }
        if (artifact) {
            artifact->metadata = std::make_shared<const MVKMetalIRMetadata>(static_cast<const MVKMetalIRMetadata&>(*artifact));
        }
        if(!artifact)owner->reportMessage(MVK_CONFIG_LOG_LEVEL_DEBUG,"MetalIR stage %u rejected: %s",execution,result.error[0]?result.error:"unsupported interface or library");
    } catch(...) {if(fromDisk)free(result.metallib);else plugin().release(&result);throw;}
    // Capture status before the compiler's release callback clears its result.
    DeviceArtifacts::Cache::Result cached{artifact,result.metallibSize,!artifact&&result.status==1};
    if(fromDisk)free(result.metallib);else plugin().release(&result);
    return cached;
    });
    if (!compiled) return reject("compiler or library did not produce a supported IR artifact");
    return compiled;
}

std::shared_ptr<MVKMetalIRArtifact> mvkCompileMetalIR(MVKPipeline* owner,MVKPipelineLayout* layout,
                                                    MVKShaderModule* module,const VkPipelineShaderStageCreateInfo* stage,uint32_t vertexTransformFlags,uint32_t runtimeOptions) {
    try {return compileMetalIR(owner,layout,module,stage,vertexTransformFlags,runtimeOptions);}
    catch(const std::exception& error) {
        owner->setConfigurationResult(owner->reportError(VK_ERROR_INITIALIZATION_FAILED,"MetalIR compilation failed: %s; MSL fallback disabled.",error.what()));
    } catch(...) {
        owner->setConfigurationResult(owner->reportError(VK_ERROR_INITIALIZATION_FAILED,"MetalIR compilation failed; MSL fallback disabled."));
    }
    return {};
}
