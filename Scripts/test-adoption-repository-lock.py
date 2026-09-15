#!/usr/bin/env python3
"""Execute the production repository acquire and converter alignment bodies.
Dependency doubles expose lock ownership, allocation failure, and refcounts;
this is not a replacement for native builds or Debug2 cold/warm runs.
"""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
def body(text, begin, end):
    start = text.index(begin)
    return text[start:text.index(end, start)]
shader = (ROOT/'MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm').read_text()
converter = (ROOT/'MoltenVKShaderConverter/MoltenVKShaderConverter/SPIRVToMSLConverter.cpp').read_text()
acquire = body(shader, 'MVKShaderLibrary* MVKShaderLibraryRepository::acquire(', '\nvoid MVKShaderLibraryRepository::release(')
adoption = body(
    shader,
    'bool MVKShaderLibraryCache::adoptShaderLibraryMembership(',
    '\nbool MVKShaderLibraryCache::merge(',
)
normalized_adoption = re.sub(r'\s+', ' ', adoption)
assert (
    '_repository->acquire( _shaderModuleKey, &alignedConfig, nullptr, true);'
    in normalized_adoption
), 'production adoption must request alignment outside the repository lock'
# The clean release source intentionally uses std::lock_guard rather than the
# diagnostics-only traced lock wrapper. Substitute an equivalent harness lock
# so the test can observe lock ownership without changing production code.
acquire = acquire.replace('lock_guard<mutex> lock(_lock);', 'mvkcachetrace::Lock lock(_lock, __func__, this);')
align = body(converter, 'MVK_PUBLIC_SYMBOL void SPIRVToMSLConversionConfiguration::alignWith(', '\n\n#pragma mark -')
source = r'''
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>
using namespace std;
#define MVK_PUBLIC_SYMBOL
using MVKShaderModuleKey = uint64_t;
static thread_local bool locked = false;
static thread_local function<void()> alignmentProbe;
static thread_local bool throwCopy = false;
namespace mvkcachetrace {
struct Scope { Scope(const char*, const void*) {} };
struct Lock {
    unique_lock<mutex> held;
    Lock(mutex& m, const char*, const void*) : held(m) { assert(!locked); locked = true; }
    ~Lock() { locked = false; }
};
}
struct Binding {
    int key = 0;
    bool outIsUsedByShader = false;
    bool matches(const Binding& b) const {
        if (alignmentProbe) { alignmentProbe(); }
        return key == b.key;
    }
};
struct SPIRVToMSLConversionConfiguration {
    int variant = 0;
    vector<Binding> shaderInputs, shaderOutputs, resourceBindings;
    SPIRVToMSLConversionConfiguration() = default;
    SPIRVToMSLConversionConfiguration(const SPIRVToMSLConversionConfiguration&) = default;
    SPIRVToMSLConversionConfiguration& operator=(const SPIRVToMSLConversionConfiguration& o) {
        if (throwCopy) { throw bad_alloc(); }
        variant = o.variant; shaderInputs = o.shaderInputs;
        shaderOutputs = o.shaderOutputs; resourceBindings = o.resourceBindings;
        return *this;
    }
    bool matches(const SPIRVToMSLConversionConfiguration& b) const { return variant == b.variant; }
    void alignWith(const SPIRVToMSLConversionConfiguration&);
};
// @ALIGN@
struct MVKShaderLibrary {
    atomic<int> references{1};
    bool resident = true;
    void* _owner = nullptr;
    void* _repository = nullptr;
    atomic<bool> _repositoryTracked{false}, _repositoryResidentCounted{false};
    void retain() { references++; }
    void release() { assert(references.fetch_sub(1) > 0); }
    bool isResident() const { return resident; }
    bool tryAdoptResidentPayload(MVKShaderLibrary*) { return false; }
};
template<class T> void updateAtomicMaximum(atomic<T>& target, T value) {
    auto old = target.load();
    while (old < value && !target.compare_exchange_weak(old, value)) {}
}
struct MVKShaderLibraryRepository {
    struct Entry { SPIRVToMSLConversionConfiguration shaderConfig; MVKShaderLibrary* library; uint32_t membershipCount; };
    mutex _lock;
    unordered_map<MVKShaderModuleKey, vector<Entry>> _entries;
    atomic<uint64_t> _logicalMembershipCount{0}, _logicalMembershipPeak{0}, _dedupeHitCount{0},
        _canonicalPublishCount{0}, _residentAdoptionCount{0}, _raceLoserCount{0};
    atomic<size_t> _residentEntryCount{0}, _residentPeakCount{0};
    void trimToResidentLimit(MVKShaderLibrary*) {}
    MVKShaderLibrary* acquire(MVKShaderModuleKey, SPIRVToMSLConversionConfiguration*, MVKShaderLibrary* = nullptr, bool = false);
};
// @ACQUIRE@
static SPIRVToMSLConversionConfiguration config(int variant) {
    SPIRVToMSLConversionConfiguration c; c.variant = variant;
    c.shaderInputs = {{0,true},{1,false}};
    c.shaderOutputs = {{2,false},{3,true}};
    // Duplicates with conflicting used flags deliberately test last-match wins.
    c.resourceBindings = {{0,true},{0,false},{1,true},{4,false}};
    return c;
}
static bool same(const vector<Binding>& a, const vector<Binding>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i=0;i<a.size();++i) if (a[i].key!=b[i].key || a[i].outIsUsedByShader!=b[i].outIsUsedByShader) return false;
    return true;
}
static void equalConfig(const SPIRVToMSLConversionConfiguration& a, const SPIRVToMSLConversionConfiguration& b) {
    assert(a.variant==b.variant && same(a.shaderInputs,b.shaderInputs) && same(a.shaderOutputs,b.shaderOutputs) && same(a.resourceBindings,b.resourceBindings));
}
int main() {
    MVKShaderLibraryRepository repo;
    MVKShaderLibrary library;
    auto canonical = config(1);
    assert(repo.acquire(7, &canonical, &library) == &library);
    assert(library.references == 2 && repo._logicalMembershipCount == 1);
    auto original = config(1); original.resourceBindings.push_back({99,true});
    auto expected = original; expected.alignWith(canonical);
    auto candidate = original;
    bool sawOutside = false;
    alignmentProbe = [&] { assert(!locked); sawOutside=true; };
    assert(repo.acquire(7, &candidate, nullptr, true) == &library);
    alignmentProbe = {};
    assert(sawOutside); equalConfig(expected, candidate);
    assert(repo._logicalMembershipCount==2 && library.references==3);
    auto foreground = original;
    bool sawInside = false;
    alignmentProbe = [&] { assert(locked); sawInside=true; };
    assert(repo.acquire(7, &foreground) == &library);
    alignmentProbe = {};
    assert(sawInside); equalConfig(expected, foreground);
    // Snapshot allocation failure occurs before any membership/reference change.
    auto refs = library.references.load(); auto memberships = repo._logicalMembershipCount.load();
    throwCopy = true;
    try { repo.acquire(7, &original, nullptr, true); assert(false); } catch (const bad_alloc&) {}
    throwCopy = false;
    assert(library.references==refs && repo._logicalMembershipCount==memberships);
    auto missing = config(2);
    assert(!repo.acquire(7,&missing,nullptr,true));
    assert(!repo.acquire(77,&original,nullptr,true));
    assert(!repo.acquire(7,nullptr,nullptr,true));
    // A cold resident payload is still an exact logical membership: no compile.
    library.resident = false;
    auto cold = original;
    assert(repo.acquire(7,&cold,nullptr,true)==&library && !library.resident);
    equalConfig(expected,cold);
    // Deterministically block adoption alignment. Foreground must still acquire
    // the same repository before the alignment is allowed to complete.
    promise<void> entered, resume;
    auto resumed = resume.get_future().share();
    auto work = async(launch::async, [&] {
        bool first = true;
        alignmentProbe = [&] { assert(!locked); if(first) { first=false; entered.set_value(); resumed.wait(); } };
        auto c = original; auto result = repo.acquire(7,&c,nullptr,true);
        alignmentProbe={}; equalConfig(expected,c); return result;
    });
    assert(entered.get_future().wait_for(chrono::seconds(2))==future_status::ready);
    auto fg = async(launch::async, [&] { auto c=original; return repo.acquire(7,&c); });
    assert(fg.wait_for(chrono::seconds(2))==future_status::ready);
    assert(fg.get()==&library);
    resume.set_value(); assert(work.get()==&library);
    // Contended producers preserve exact count/ref balance.
    vector<thread> threads;
    for(int t=0;t<4;t++) threads.emplace_back([&] { for(int i=0;i<100;i++) { auto c=original; assert(repo.acquire(7,&c,nullptr,true)==&library); equalConfig(expected,c); } });
    for(auto& t:threads)t.join();
    assert(repo._entries.at(7)[0].membershipCount==repo._logicalMembershipCount);
    assert(library.references==int(repo._logicalMembershipCount+1));
    cout << "PASS: exact production acquire/alignment; canonical equivalence, duplicate bindings, copy failure, misses, cold payload, concurrent foreground progress, reference balance\n";
}
'''.replace('// @ALIGN@',align).replace('// @ACQUIRE@',acquire)
with tempfile.TemporaryDirectory(prefix='mvk-adoption-lock-') as tmp:
    cpp=Path(tmp)/'test.cpp'; exe=Path(tmp)/'test'; cpp.write_text(source)
    subprocess.run(['clang++','-std=c++17','-O1','-pthread','-fsanitize=address,undefined',str(cpp),'-o',str(exe)], check=True, timeout=40)
    subprocess.run([str(exe)], check=True, timeout=20)
print(json.dumps(dict(
    acquireSha256=hashlib.sha256(acquire.encode()).hexdigest(),
    adoptionWiringSha256=hashlib.sha256(adoption.encode()).hexdigest(),
    alignmentSha256=hashlib.sha256(align.encode()).hexdigest(),
)))
