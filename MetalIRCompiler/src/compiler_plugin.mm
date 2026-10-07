#include "MVKMetalIRBridge.h"
#include "spirv_to_dxil.h"
#include "native_raster_adapter.h"
#include "air_math_adapter.h"
#include <metal_irconverter/metal_irconverter.h>
#include <TargetConditionals.h>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <strings.h>
#include <cstdlib>
#include <cstdio>
#include <chrono>
#include <time.h>

#ifndef MELONX_METAL_IR_DEPENDENCY_IDENTITY
#error "Build with the SHA-256 identity of the pinned Mesa and MSC binaries"
#endif
extern "C" __attribute__((visibility("default")))
const char* MeloNXMetalIRDependencyIdentity() { return MELONX_METAL_IR_DEPENDENCY_IDENTITY; }
extern "C" __attribute__((visibility("default")))
uint32_t MeloNXMetalIRABIVersion() { return MVK_METAL_IR_ABI_VERSION; }

static double elapsed(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
}
static void logMesa(void* output,const char* message) {
    auto* result=(MVKMetalIRCompileResult*)output;
    snprintf(result->error,sizeof(result->error),"%s",message);
}
struct ReplayCompilerPhase {
    const char* region;uint32_t stage;uint64_t wall=0,cpu=0;
    static uint64_t read(clockid_t clock) {timespec t{};return clock_gettime(clock,&t)==0?uint64_t(t.tv_sec)*1000000000+t.tv_nsec:0;}
    ReplayCompilerPhase(const char* r,uint32_t s):region(r),stage(s) {
        const char* option=getenv("MELONX_PIPELINE_REPLAY_TRACE");
        if(option&&strcmp(option,"1")==0){wall=read(CLOCK_MONOTONIC);cpu=read(CLOCK_THREAD_CPUTIME_ID);}
    }
    ~ReplayCompilerPhase() {if(wall){uint64_t endCpu=read(CLOCK_THREAD_CPUTIME_ID);fprintf(stderr,"IR_REPLAY_PHASE region=%s stage=%u wall_ns=%llu thread_cpu_ns=%llu cpu_valid=%u\n",region,stage,(unsigned long long)(read(CLOCK_MONOTONIC)-wall),(unsigned long long)(cpu&&endCpu>=cpu?endCpu-cpu:0),cpu&&endCpu>=cpu);}}
};
struct CompileResources {
    dxil_spirv_object dxil={};
    IRError* error=nullptr;
    IRRootSignature* root=nullptr;
    IRObject* input=nullptr;
    IRObject* output=nullptr;
    IRCompiler* compiler=nullptr;
    IRMetalLibBinary* binary=nullptr;
    IRShaderReflection* reflection=nullptr;
    ~CompileResources() {
        if(reflection)IRShaderReflectionDestroy(reflection);
        if(binary)IRMetalLibBinaryDestroy(binary);
        if(output)IRObjectDestroy(output);
        if(compiler)IRCompilerDestroy(compiler);
        if(input)IRObjectDestroy(input);
        if(root)IRRootSignatureDestroy(root);
        if(error)IRErrorDestroy(error);
        spirv_to_dxil_free(&dxil);
    }
};
extern "C" __attribute__((visibility("default")))
void MeloNXReleaseMetalIR(MVKMetalIRCompileResult* result) {
    if(result){free(result->metallib);result->metallib=nullptr;result->metallibSize=0;}
}
extern "C" __attribute__((visibility("default")))
int MeloNXCompileMetalIR(const MVKMetalIRCompileRequest* request,MVKMetalIRCompileResult* result) {
    if(!request||!result||request->abiVersion!=MVK_METAL_IR_ABI_VERSION)return 1;
    *result={};result->abiVersion=MVK_METAL_IR_ABI_VERSION;result->status=1;
    memset(result->vertexAttributes,255,sizeof(result->vertexAttributes));
    if(request->executionModel!=0&&request->executionModel!=4&&request->executionModel!=5){result->status=1;return 1;}
    if(request->wordCount<5||request->setCount>8||request->mathMode>MVK_METAL_IR_MATH_RELAXED)return 1;
    std::vector<uint32_t> words(request->words,request->words+request->wordCount);
    std::unordered_map<uint32_t,uint32_t> sets,bindings;
    for(size_t pos=5;pos<words.size();) {
        uint32_t count=words[pos]>>16,op=words[pos]&65535;
        if(!count||pos+count>words.size())return 1;
        if(op==71&&count>=4){
            if(words[pos+2]==34)sets[words[pos+1]]=words[pos+3];
            if(words[pos+2]==33)bindings[words[pos+1]]=words[pos+3];
        }
        pos+=count;
    }
    for(size_t pos=5;pos<words.size();) {
        uint32_t count=words[pos]>>16,op=words[pos]&65535;
        if(op==71&&count>=4&&words[pos+2]==33) {
            uint32_t id=words[pos+1];auto set=sets.find(id);
            if(set==sets.end())return 1;
            bool found=false;
            for(size_t i=0;i<request->bindingCount;++i) {
                const auto& b=request->bindings[i];
                if(b.set==set->second&&b.binding==words[pos+3]){words[pos+3]=b.denseIndex;found=true;break;}
            }
            if(!found){snprintf(result->error,sizeof(result->error),"descriptor %u/%u missing from layout",set->second,words[pos+3]);return 1;}
        }
        pos+=count;
    }
    dxil_spirv_runtime_conf conf={};
    conf.runtime_data_cbv.register_space=31;conf.push_constant_cbv.register_space=30;
    conf.runtime_data_srv.register_space=28;conf.runtime_data_srv.enabled=true;
    conf.first_vertex_and_base_instance_mode=DXIL_SPIRV_SYSVAL_TYPE_RUNTIME_DATA;
    // Ordinary dispatch has a known zero base; base-enabled pipelines receive
    // the actual Vulkan origin in the runtime CBV, not Metal stage-in state.
    conf.workgroup_id_mode=request->runtimeOptions&MVK_METAL_IR_ALLOW_DISPATCH_BASE
        ?DXIL_SPIRV_SYSVAL_TYPE_RUNTIME_DATA:DXIL_SPIRV_SYSVAL_TYPE_ZERO;
    conf.shader_model_max=SHADER_MODEL_6_6;conf.keep_io_vars=true;
    conf.disable_math_refactoring=request->mathMode!=MVK_METAL_IR_MATH_FAST;
    conf.relaxed_math_refactoring=request->mathMode==MVK_METAL_IR_MATH_RELAXED;
    if(request->executionModel==0&&(request->vertexTransformFlags&MVK_METAL_IR_FLIP_Y)) {
        conf.yz_flip.mode=DXIL_SPIRV_Y_FLIP_UNCONDITIONAL;
        conf.yz_flip.y_mask=UINT16_MAX;
    }
    // Explicit descriptor tables retain Vulkan resource kinds. The native
    // encoder writes 24-byte IR entries directly into these tables.
    conf.lower_to_bindless=false;
    dxil_spirv_debug_options debug={};dxil_spirv_logger logger={result,logMesa};
    CompileResources resources;
    auto& dxil=resources.dxil;
    auto start=std::chrono::steady_clock::now();
    auto stage=request->executionModel==0?DXIL_SPIRV_SHADER_VERTEX:request->executionModel==4?DXIL_SPIRV_SHADER_FRAGMENT:DXIL_SPIRV_SHADER_COMPUTE;
    uint32_t nativeRasterIO=0;
    bool clipHalfZ=request->executionModel==0&&(request->vertexTransformFlags&MVK_METAL_IR_CLIP_HALF_Z);
    // Metal forbids point_size on triangle/line pipelines. Select raster I/O
    // from the Vulkan pipeline topology before compiling, just as the MSL
    // path selects enable_point_size_builtin. Both branches use Mesa + MSC.
    bool translated=request->runtimeOptions&MVK_METAL_IR_RENDERING_POINTS
        ?spirv_to_dxil_with_native_raster(words.data(),words.size(),nullptr,0,stage,request->entry,NO_DXIL_VALIDATION,&debug,&conf,&logger,&dxil,clipHalfZ,&nativeRasterIO)
        :spirv_to_dxil_with_clip_space(words.data(),words.size(),nullptr,0,stage,request->entry,NO_DXIL_VALIDATION,&debug,&conf,&logger,&dxil,clipHalfZ);
    result->mesaMs=elapsed(start);
    if(!translated)return 1;
    // Mesa runtime data and MSC draw arguments have separate ABIs. The native
    // encoder supplies both, and this metadata must survive disk restoration.
    bool runtimeData=dxil.metadata.requires_runtime_data;
    bool runtimeRaw=dxil.metadata.requires_runtime_srv;
    if(runtimeRaw)result->runtimeFlags|=request->executionModel==0?MVK_METAL_IR_DRAW_BASES:MVK_METAL_IR_DISPATCH_GROUPS;
    if(runtimeData&&request->executionModel!=0&&request->executionModel!=5){snprintf(result->error,sizeof(result->error),"runtime data for unsupported stage");return 1;}
    if(runtimeData)result->runtimeFlags|=MVK_METAL_IR_RUNTIME_DATA;
    if(request->executionModel==0&&dxil.metadata.unit_point_size)
        result->runtimeFlags|=MVK_METAL_IR_UNIT_POINT_SIZE;
    std::vector<IRDescriptorRange1> ranges(request->setCount*4);
    std::vector<IRRootParameter1> params(request->setCount*2+(request->pushConstantSize?1:0)+(runtimeData?1:0)+(runtimeRaw?1:0));
    for(uint32_t set=0;set<request->setCount;++set) {
        uint32_t n=request->setSizes[set];
        for(uint32_t type=0;type<3;++type) {
            auto& r=ranges[set*4+type];r.RangeType=type==0?IRDescriptorRangeTypeCBV:type==1?IRDescriptorRangeTypeSRV:IRDescriptorRangeTypeUAV;
            r.NumDescriptors=n?n:1;r.RegisterSpace=set;r.OffsetInDescriptorsFromTableStart=type*n;
        }
        auto& sampler=ranges[set*4+3];sampler.RangeType=IRDescriptorRangeTypeSampler;sampler.NumDescriptors=n?n:1;sampler.RegisterSpace=set;
        auto& resourceParam=params[set*2];resourceParam.ParameterType=IRRootParameterTypeDescriptorTable;resourceParam.ShaderVisibility=IRShaderVisibilityAll;resourceParam.DescriptorTable={3,&ranges[set*4]};
        auto& samplerParam=params[set*2+1];samplerParam.ParameterType=IRRootParameterTypeDescriptorTable;samplerParam.ShaderVisibility=IRShaderVisibilityAll;samplerParam.DescriptorTable={1,&ranges[set*4+3]};
    }
    if(request->pushConstantSize) {
        auto& push=params[request->setCount*2];push.ParameterType=IRRootParameterTypeCBV;push.ShaderVisibility=IRShaderVisibilityAll;
        push.Descriptor={0,30,IRRootDescriptorFlagDataVolatile};
    }
    if(runtimeData) {
        auto& runtime=params[request->setCount*2+(request->pushConstantSize?1:0)];runtime.ParameterType=IRRootParameterTypeCBV;runtime.ShaderVisibility=IRShaderVisibilityAll;
        runtime.Descriptor={0,31,IRRootDescriptorFlagDataVolatile};
    }
    if(runtimeRaw) {
        auto& raw=params.back();raw.ParameterType=IRRootParameterTypeSRV;raw.ShaderVisibility=IRShaderVisibilityAll;
        raw.Descriptor={0,28,IRRootDescriptorFlagDataVolatile};
    }
    IRVersionedRootSignatureDescriptor rd={};rd.version=IRRootSignatureVersion_1_1;rd.desc_1_1.NumParameters=params.size();rd.desc_1_1.pParameters=params.data();
    auto& error=resources.error;auto& root=resources.root;
    { ReplayCompilerPhase trace("root_signature",request->executionModel); root=IRRootSignatureCreateFromDescriptor(&rd,&error); }
    auto& input=resources.input;auto& output=resources.output;auto& compiler=resources.compiler;
    auto& binary=resources.binary;auto& reflection=resources.reflection;
    int status=2;
    if(root) {
        input=IRObjectCreateFromDXIL((const uint8_t*)dxil.binary.buffer,dxil.binary.size,IRBytecodeOwnershipNone);
        compiler=IRCompilerCreate();IRCompilerSetGlobalRootSignature(compiler,root);IRCompilerIgnoreRootSignature(compiler,true);
#if TARGET_OS_IPHONE
        IRCompilerSetMinimumDeploymentTarget(compiler,IROperatingSystem_iOS,"17.0");
#else
        IRCompilerSetMinimumDeploymentTarget(compiler,IROperatingSystem_macOS,"26.0");
#endif
        IRCompilerSetMinimumGPUFamily(compiler,IRGPUFamilyMetal3);
        uint32_t flags=0;
#if IR_SUPPORTS_VERSION(4, 0, 0)
        // MSC 4 introduced default NaN/Inf optimization. Earlier MSC versions
        // have no opt-out flag; their behavior is covered by the NaN/Inf oracle.
        if(request->mathMode!=MVK_METAL_IR_MATH_FAST)flags|=IRCompatibilityFlagDisableNanInfOptimization;
#endif
        if(request->preserveInvariance)flags|=IRCompatibilityFlagPositionInvariance;
        IRCompilerSetCompatibilityFlags(compiler,(IRCompatibilityFlags)flags);
        start=std::chrono::steady_clock::now(); { ReplayCompilerPhase trace("apple_converter",request->executionModel); output=IRCompilerAllocCompileAndLink(compiler,nullptr,input,&error); } result->converterMs=elapsed(start);
        if(output) {
            auto actualStage=IRObjectGetMetalIRShaderStage(output);
            reflection=IRShaderReflectionCreate();
            if(IRObjectGetReflection(output,actualStage,reflection)) {
                const char* name=IRShaderReflectionGetEntryPointFunctionName(reflection);
                if(name&&name[0]) {
                    snprintf(result->entry,sizeof(result->entry),"%s",name);
                    binary=IRMetalLibBinaryCreate();
                    if(IRObjectGetMetalLibBinary(output,actualStage,binary)) {
                        ReplayCompilerPhase trace("metallib_extract_and_raster_io",request->executionModel);
                        result->metallibSize=IRMetalLibGetBytecodeSize(binary);result->metallib=malloc(result->metallibSize);
                        if(result->metallib&&IRMetalLibGetBytecode(binary,(uint8_t*)result->metallib)==result->metallibSize)status=0;
                        if(status==0&&request->mathMode==MVK_METAL_IR_MATH_RELAXED) {
                            std::vector<uint8_t> adapted;std::string error;
                            start=std::chrono::steady_clock::now();
                            bool valid=melonx::air::relaxMathPermissions(result->metallib,result->metallibSize,adapted,error);
                            result->rasterAdapterMs=elapsed(start);
                            if(!valid) {status=1;snprintf(result->error,sizeof(result->error),"math permissions adapter: %s",error.c_str());}
                            else if(void* bytes=malloc(adapted.size())) {
                                memcpy(bytes,adapted.data(),adapted.size());free(result->metallib);
                                result->metallib=bytes;result->metallibSize=adapted.size();
                            } else {status=2;snprintf(result->error,sizeof(result->error),"math permissions adapter allocation failed");}
                        }
                        if(status==0&&nativeRasterIO) {
                            std::vector<uint8_t> adapted;std::string error;
                            start=std::chrono::steady_clock::now();
                            bool restored=melonx::air::restoreNativeRasterIO(result->metallib,result->metallibSize,request->executionModel,nativeRasterIO,adapted,error);
                            result->rasterAdapterMs+=elapsed(start);
                            if(!restored) {status=1;snprintf(result->error,sizeof(result->error),"native raster adapter: %s",error.c_str());}
                            else if(void* bytes=malloc(adapted.size())) {
                                memcpy(bytes,adapted.data(),adapted.size());free(result->metallib);
                                result->metallib=bytes;result->metallibSize=adapted.size();
                                if(nativeRasterIO&melonx::air::PointSize)result->runtimeFlags|=MVK_METAL_IR_NATIVE_POINT_SIZE;
                                if(nativeRasterIO&melonx::air::PointCoordinates)result->runtimeFlags|=MVK_METAL_IR_NATIVE_POINT_COORDINATES;
                            } else {status=2;snprintf(result->error,sizeof(result->error),"native raster adapter allocation failed");}
                        }
                    }
                    if(actualStage==IRShaderStageCompute) {
                        IRVersionedCSInfo cs={};
                        if(IRShaderReflectionCopyComputeInfo(reflection,IRReflectionVersion_1_0,&cs)) {
                            for(int i=0;i<3;++i)result->threadgroupSize[i]=cs.info_1_0.tg_size[i];
                            IRShaderReflectionReleaseComputeInfo(&cs);
                        } else {status=1;snprintf(result->error,sizeof(result->error),"compute threadgroup reflection unavailable");}
                    }
                    if(actualStage==IRShaderStageVertex) {
                        IRVersionedVSInfo vs={};
                        if(!IRShaderReflectionCopyVertexInfo(reflection,IRReflectionVersion_1_0,&vs))status=1;
                        else {
                            if(vs.info_1_0.needs_draw_params)result->runtimeFlags|=MVK_METAL_IR_DRAW_PARAMETERS;
                            for(size_t i=0;i<vs.info_1_0.num_vertex_inputs;++i){
                                const auto& input=vs.info_1_0.vertex_inputs[i];
                                char* end=nullptr;
                                unsigned long location=input.name&&strncasecmp(input.name,"texcoord",8)==0?strtoul(input.name+8,&end,10):32;
                                // MSC reflection indices are relative to the pinned
                                // runtime's kIRStageInAttributeStartIndex (11).
                                constexpr uint32_t stageInAttributeStart=11;
                                if(location<32&&end&&*end==0&&input.attributeIndex+stageInAttributeStart<32){
                                    result->vertexAttributes[location]=input.attributeIndex+stageInAttributeStart;
                                    result->vertexLocations|=1ull<<location;
                                }else{status=1;snprintf(result->error,sizeof(result->error),"unsupported MSC vertex semantic");}
                            }
                            IRShaderReflectionReleaseVertexInfo(&vs);
                        }
                    }
                }
            }
        }
    }
    if(error){const char* text=(const char*)IRErrorGetPayload(error);snprintf(result->error,sizeof(result->error),"MSC %u: %s",IRErrorGetCode(error),text?text:"");}
    result->status=status;if(status)MeloNXReleaseMetalIR(result);return status;
}
