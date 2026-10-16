// Offline ABI-9 compilation. The minimal layout is derived from this module,
// not recovered from a game's pipeline layout. No Vulkan/Metal device is used.
#include "MVKMetalIRBridge.h"
#include "spirv_msl.hpp"
#include <algorithm>
#include <array>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <mach-o/dyld.h>
namespace sc=SPIRV_CROSS_NAMESPACE;
namespace fs=std::filesystem;
static std::string quote(const std::string& value) {
    std::ostringstream s;s<<'"';
    for(unsigned char c:value) {if(c=='"'||c=='\\')s<<'\\'<<c;else if(c<32){char b[7];snprintf(b,sizeof(b),"\\u%04x",c);s<<b;}else s<<c;}
    s<<'"';return s.str();
}
static void write(const fs::path& path,const void* bytes,size_t size) {
    std::ofstream f(path,std::ios::binary);f.write(static_cast<const char*>(bytes),size);
    if(!f)throw std::runtime_error("artifact write failed");
}
static uint32_t count(sc::Compiler& compiler,const sc::Resource& resource) {
    const auto& type=compiler.get_type(resource.type_id);uint64_t n=1;
    for(size_t i=0;i<type.array.size();++i) {
        if(!type.array_size_literal[i])throw std::runtime_error("fixture: specialization-dependent descriptor count");
        if(!type.array[i])throw std::runtime_error("fixture: runtime descriptor array needs a pipeline layout");
        n*=type.array[i];if(n>1048576)throw std::runtime_error("fixture: descriptor count budget");
    }
    return uint32_t(n);
}
static bool metal_loaded() {
    for(uint32_t i=0;i<_dyld_image_count();++i){std::string p=_dyld_get_image_name(i);if(p.find("/Metal.framework/")!=p.npos||p.find("/IOGPU.framework/")!=p.npos)return true;}
    return false;
}
static std::string loaded(const char* name) {
    std::string found;
    for(uint32_t i=0;i<_dyld_image_count();++i) {
        fs::path p=_dyld_get_image_name(i);
        if(p.filename()==name) {if(!found.empty())throw std::runtime_error("duplicate compiler dependency");found=fs::canonical(p).string();}
    }
    return found;
}
int main(int argc,char** argv) {
    try {
        auto guard=reinterpret_cast<int(*)()>(dlsym(RTLD_DEFAULT,"B4NoMetalGuardActive"));
        if(!guard)throw std::runtime_error("offline guard is not active");
        bool queued=guard()==2;
        if(!queued&&guard()!=1)throw std::runtime_error("invalid guard mode");
        if(argc==2 && std::string(argv[1])=="guard-test") {
            dlopen("/System/Library/Frameworks/Metal.framework/Metal",RTLD_LAZY);
            throw std::runtime_error("offline guard failed its positive control");
        }
        if(argc!=6)return 64;
        std::string route=argv[1];unsigned mode=std::stoul(argv[2]);
        if(mode>2||(route!="IR"&&route!="MSL"))return 64;
        fs::path plugin=fs::canonical(argv[3]),source=argv[4],out=argv[5];
        std::ifstream file(source,std::ios::binary|std::ios::ate);auto bytes=file.tellg();
        if(bytes<20||bytes>16*1024*1024||bytes%4)throw std::runtime_error("invalid input extent");
        file.seekg(0);std::vector<uint32_t> words(size_t(bytes)/4);file.read(reinterpret_cast<char*>(words.data()),bytes);
        if(!file)throw std::runtime_error("input read failed");
        sc::CompilerMSL compiler(words);auto entries=compiler.get_entry_points_and_stages();
        if(entries.size()!=1)throw std::runtime_error("fixture: expected one entry point");
        const auto& entry=entries[0];uint32_t model=entry.execution_model;
        compiler.set_entry_point(entry.name,entry.execution_model);
        compiler.update_active_builtins();
        bool pointFragment=model==4&&compiler.has_active_builtin(spv::BuiltInPointCoord,spv::StorageClassInput);
        auto resources=compiler.get_shader_resources();
        struct Binding {MVKMetalIRBinding abi{};uint32_t mask=0;};
        std::map<std::pair<uint32_t,uint32_t>,Binding> layout;
        auto add=[&](const auto& list,uint32_t type,uint32_t mask){for(const auto& resource:list) {
            uint32_t set=compiler.get_decoration(resource.id,spv::DecorationDescriptorSet);
            uint32_t binding=compiler.get_decoration(resource.id,spv::DecorationBinding);
            if(set>=8)throw std::runtime_error("fixture: descriptor set exceeds ABI 9");
            Binding b;b.abi={set,binding,count(compiler,resource),type,0,{UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX}};b.mask=mask;
            if(!layout.emplace(std::make_pair(set,binding),b).second)throw std::runtime_error("fixture: alias descriptor declarations need pipeline metadata");
        }};
        add(resources.uniform_buffers,6,1);add(resources.storage_buffers,7,6);
        add(resources.sampled_images,1,10);add(resources.separate_images,2,2);
        add(resources.separate_samplers,0,8);add(resources.storage_images,3,6);add(resources.subpass_inputs,10,2);
        std::vector<MVKMetalIRBinding> bindings;std::vector<uint32_t> sizes;
        uint32_t setCount=layout.empty()?0:layout.rbegin()->first.first+1;
        for(uint32_t set=0;set<setCount;++set) {
            uint32_t dense=0,counts[4]={};size_t first=bindings.size();
            for(auto& pair:layout)if(pair.first.first==set) {
                auto& item=pair.second;item.abi.denseIndex=dense;dense+=item.abi.count;
                for(unsigned kind=0;kind<4;++kind)if(item.mask&(1u<<kind)){item.abi.tableOffsets[kind]=counts[kind];counts[kind]+=item.abi.count;}
                bindings.push_back(item.abi);
            }
            uint32_t base=0;for(unsigned kind=0;kind<4;++kind){for(size_t i=first;i<bindings.size();++i)if(bindings[i].tableOffsets[kind]!=UINT32_MAX)bindings[i].tableOffsets[kind]+=base;base+=counts[kind];}
            sizes.push_back(dense);
        }
        uint32_t push=0;
        for(const auto& buffer:resources.push_constant_buffers)push=std::max(push,uint32_t(compiler.get_declared_struct_size(compiler.get_type(buffer.base_type_id))));
        if(route=="MSL") {
            auto options=compiler.get_msl_options();options.platform=sc::CompilerMSL::Options::macOS;
            options.msl_version=sc::CompilerMSL::Options::make_msl_version(4,0);
            options.argument_buffers=true;options.argument_buffers_tier=sc::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
            options.texture_buffer_native=true;options.force_active_argument_buffer_resources=false;
            options.enable_point_size_builtin=false;options.emulate_subgroups=false;options.fixed_subgroup_size=32;
            compiler.set_msl_options(options);
            auto text=compiler.compile();write(out,text.data(),text.size());
            std::cout<<"{\"status\":0,\"route\":\"MSL\",\"mode\":"<<mode<<",\"modeAppliedBy\":\"not applied in source generation; Metal compilation not run\",\"stage\":"<<model<<",\"bytes\":"<<text.size()<<",\"metalLoaded\":"<<(metal_loaded()?"true":"false")<<"}\n";
            return metal_loaded()&&!queued?86:0;
        }
        void* library=dlopen(plugin.c_str(),RTLD_NOW|RTLD_LOCAL);
        if(!library)throw std::runtime_error(dlerror());
        auto abi=reinterpret_cast<uint32_t(*)()>(dlsym(library,"MeloNXMetalIRABIVersion"));
        auto compile=reinterpret_cast<MVKMetalIRCompileFunction>(dlsym(library,"MeloNXCompileMetalIR"));
        auto release=reinterpret_cast<MVKMetalIRReleaseFunction>(dlsym(library,"MeloNXReleaseMetalIR"));
        auto identity=reinterpret_cast<const char*(*)()>(dlsym(library,"MeloNXMetalIRDependencyIdentity"));
        if(!abi||abi()!=9||!compile||!release||!identity)throw std::runtime_error("compiler ABI mismatch");
        bool verified=true;for(const char* name:{"libMeloNXMetalIR.dylib","libspirv_to_dxil.dylib","libmetalirconverter.dylib"})
            verified &= loaded(name)==fs::canonical(plugin.parent_path()/name).string();
        if(!verified)throw std::runtime_error("loaded compiler dependency mismatch");
        // A consumed PointCoord needs the point-rasterization variant. The host
        // cache has no pipeline topology, so record this necessary fixture choice.
        uint32_t runtimeOptions=pointFragment?MVK_METAL_IR_RENDERING_POINTS:0;
        MVKMetalIRCompileRequest request={9,model,words.data(),words.size(),entry.name.c_str(),bindings.data(),bindings.size(),sizes.data(),setCount,push,model==0,mode,0,runtimeOptions};
        MVKMetalIRCompileResult result{};int code=compile(&request,&result);
        if(!code&&result.metallib&&result.metallibSize)write(out,result.metallib,result.metallibSize);
        else if(!code)code=2;
        std::cout<<"{\"status\":"<<code<<",\"route\":\"IR\",\"mode\":"<<mode<<",\"stage\":"<<model<<",\"bytes\":"<<result.metallibSize
                 <<",\"mesaMs\":"<<result.mesaMs<<",\"converterMs\":"<<result.converterMs<<",\"adapterMs\":"<<result.rasterAdapterMs
                 <<",\"runtimeFlags\":"<<result.runtimeFlags<<",\"bindingCount\":"<<bindings.size()<<",\"setCount\":"<<setCount
                 <<",\"pointRasterFixture\":"<<(pointFragment?"true":"false")
                 <<",\"dependencyIdentity\":"<<quote(identity())<<",\"error\":"<<quote(result.error)<<",\"metalLoaded\":"<<(metal_loaded()?"true":"false")<<",\"loadedDependenciesVerified\":true}\n";
        release(&result);return code?1:metal_loaded()&&!queued?86:0;
    } catch(const std::exception& error) {
        std::cout<<"{\"status\":1,\"error\":"<<quote(error.what())<<"}\n";return 2;
    }
}
