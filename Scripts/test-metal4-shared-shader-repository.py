#!/usr/bin/env python3
"""Static acceptance gate for the Metal 4 shared shader-library repository.

The production implementation is Objective-C++ and requires an Apple SDK to
compile. This script protects ownership, logical cache-view, cold/resident,
and compile-required invariants in every environment. It complements the
normal Xcode and CMake builds; it does not replace them.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DEVICE_H = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKDevice.h"
DEVICE_MM = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm"
PIPELINE_MM = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm"
PIPELINE_H = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h"
SHADER_H = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.h"
SHADER_MM = ROOT / "MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm"
PRIVATE_API_H = ROOT / "MoltenVK/MoltenVK/API/mvk_private_api.h"
API_MM = ROOT / "MoltenVK/MoltenVK/Vulkan/mvk_api.mm"
CONFIG_MEMBERS = ROOT / "MoltenVK/MoltenVK/Utility/MVKConfigMembers.def"
ENVIRONMENT_H = ROOT / "MoltenVK/MoltenVK/Utility/MVKEnvironment.h"


def require(text: str, needle: str, source: Path) -> None:
    if needle not in text:
        raise AssertionError(f"{source}: missing required invariant: {needle}")


@dataclass
class Entry:
    memberships: int = 0
    repository_ref: int = 1
    view_refs: int = 0
    resident: bool = True
    resident_counted: bool = True
    rehydrate_count: int = 0

    def acquire(self) -> None:
        self.memberships += 1
        self.view_refs += 1

    def release(self) -> None:
        assert self.memberships > 0
        assert self.view_refs > 0
        self.memberships -= 1
        self.view_refs -= 1
        if self.memberships == 0:
            self.repository_ref -= 1
            self.resident_counted = False

    def evict_resident(self) -> None:
        assert self.memberships > 0
        self.resident = False
        self.resident_counted = False

    def get_function(self, allow_compile: bool) -> bool:
        if self.resident:
            return True
        if not allow_compile:
            return False
        self.resident = True
        self.resident_counted = True
        self.rehydrate_count += 1
        return True


@dataclass(frozen=True)
class PipelineContribution:
    shader_module_key: str
    shader_config: str
    shader_library: str


class LogicalCacheView:
    def __init__(self) -> None:
        self.memberships: set[tuple[str, str, str]] = set()

    def adopt_pipeline(self, contributions: list[PipelineContribution]) -> int:
        adopted = 0
        for contribution in contributions:
            membership = (
                contribution.shader_module_key,
                contribution.shader_config,
                contribution.shader_library,
            )
            if membership not in self.memberships:
                self.memberships.add(membership)
                adopted += 1
        return adopted


class OneShotCapture:
    def __init__(self) -> None:
        self.source: str | None = None
        self.token = 0
        self.next_token = 1

    def begin(self, source: str) -> int | None:
        if self.source is not None:
            return None
        self.source = source
        self.token = self.next_token
        self.next_token += 1
        return self.token

    def consume_for_pipeline(self, source: str) -> bool:
        if self.source != source:
            return False
        self.source = None
        self.token = 0
        return True

    def cancel(self, token: int) -> None:
        if token == self.token:
            self.source = None
            self.token = 0


def test_reference_and_residency_model() -> None:
    entry = Entry()
    entry.acquire()  # global cache membership
    entry.acquire()  # per-program MSL cache membership
    assert entry.memberships == 2
    assert entry.repository_ref + entry.view_refs == 3

    # Resident eviction must not remove either logical cache membership.
    entry.evict_resident()
    assert entry.memberships == 2
    assert entry.repository_ref + entry.view_refs == 3

    # VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT must not
    # silently rehydrate an evicted Metal library.
    assert not entry.get_function(allow_compile=False)
    assert not entry.resident

    # A normal pipeline request may rehydrate from retained compressed MSL.
    assert entry.get_function(allow_compile=True)
    assert entry.resident
    assert entry.rehydrate_count == 1

    entry.release()  # per-program cache is recycled
    assert entry.memberships == 1
    assert entry.repository_ref == 1
    assert entry.view_refs == 1

    entry.release()  # global view is destroyed
    assert entry.memberships == 0
    assert entry.repository_ref == 0
    assert entry.view_refs == 0
    assert not entry.resident_counted


def test_exact_pipeline_contribution_adoption_model() -> None:
    destination = LogicalCacheView()
    pipeline_a = [
        PipelineContribution("vertex-a", "config-1", "library-1"),
        PipelineContribution("fragment-a", "config-2", "library-2"),
        PipelineContribution("vertex-a", "config-1", "library-1"),
    ]
    unrelated_pipeline = PipelineContribution("compute-b", "config-3", "library-3")

    # Adoption adds only the exact libraries that contributed to this Pipeline.
    assert destination.adopt_pipeline(pipeline_a) == 2
    assert (
        unrelated_pipeline.shader_module_key,
        unrelated_pipeline.shader_config,
        unrelated_pipeline.shader_library,
    ) not in destination.memberships

    # Re-adoption is a dedupe hit, not another logical membership or count.
    assert destination.adopt_pipeline(pipeline_a) == 0
    assert len(destination.memberships) == 2


def test_one_shot_capture_model() -> None:
    capture = OneShotCapture()
    token = capture.begin("global-a")
    assert token is not None
    assert capture.begin("global-a") is None  # Nested begin is rejected.
    assert not capture.consume_for_pipeline("per-program-b")
    assert capture.consume_for_pipeline("global-a")
    assert not capture.consume_for_pipeline("global-a")

    token = capture.begin("global-a")
    assert token is not None
    capture.cancel(token)
    capture.cancel(token)  # Idempotent finally cleanup.
    assert not capture.consume_for_pipeline("global-a")


def test_source_policy() -> None:
    device_h = DEVICE_H.read_text()
    device_mm = DEVICE_MM.read_text()
    pipeline_mm = PIPELINE_MM.read_text()
    pipeline_h = PIPELINE_H.read_text()
    shader_h = SHADER_H.read_text()
    shader_mm = SHADER_MM.read_text()
    private_api_h = PRIVATE_API_H.read_text()
    api_mm = API_MM.read_text()
    config_members = CONFIG_MEMBERS.read_text()
    environment_h = ENVIRONMENT_H.read_text()

    require(device_h, "MVKShaderLibraryRepository* getShaderLibraryRepository() const", DEVICE_H)
    require(device_h, "MVKShaderLibraryRepository* _shaderLibraryRepository = nullptr;", DEVICE_H)
    require(
        device_mm,
        "? MVKShaderLibraryRepository::create(this)",
        DEVICE_MM,
    )
    require(device_mm, "delete _shaderLibraryRepository;\n\tdelete _metal4CompilerService;", DEVICE_MM)

    require(pipeline_mm, "new MVKShaderLibraryCache(this, smKey)", PIPELINE_MM)
    require(shader_h, "class MVKShaderLibraryRepository", SHADER_H)
    require(shader_h, "std::atomic<uint32_t> _referenceCount", SHADER_H)
    require(shader_h, "std::atomic<bool> _resident", SHADER_H)
    require(shader_h, "std::atomic<bool> _repositoryResidentCounted", SHADER_H)
    require(shader_h, "std::atomic<bool> _repositoryTracked", SHADER_H)
    require(shader_h, "std::atomic<uint64_t> _lastUseSequence", SHADER_H)
    require(shader_h, "std::atomic<uint32_t> _activeUseCount", SHADER_H)
    require(shader_h, "std::atomic<uint64_t> _residentAdoptionCount", SHADER_H)
    require(shader_h, "_lastRehydrateNanoseconds", SHADER_H)
    require(shader_h, "_rehydrateProtectedUntilSequence", SHADER_H)
    require(shader_h, "MVKShaderLibraryEvictionSnapshot", SHADER_H)
    require(shader_h, "std::mutex _accessLock", SHADER_H)

    # Repository activation is part of the native MoltenVK configuration ABI.
    # The default remains disabled for generic users. MeloNX sets enabled=true and
    # a resident limit through vkSetMoltenVKConfigurationMVK before VkDevice creation.
    require(
        private_api_h,
        "metal4SharedShaderLibraryRepositoryEnabled",
        PRIVATE_API_H,
    )
    require(
        private_api_h,
        "metal4SharedShaderLibraryResidentLimit",
        PRIVATE_API_H,
    )
    require(
        config_members,
        "METAL4_SHARED_SHADER_LIBRARY_REPOSITORY_ENABLED",
        CONFIG_MEMBERS,
    )
    require(
        config_members,
        "METAL4_SHARED_SHADER_LIBRARY_RESIDENT_LIMIT",
        CONFIG_MEMBERS,
    )
    require(
        environment_h,
        "#   define MVK_CONFIG_METAL4_SHARED_SHADER_LIBRARY_REPOSITORY_ENABLED 0",
        ENVIRONMENT_H,
    )
    require(
        shader_mm,
        "device->getMVKConfig()",
        SHADER_MM,
    )
    require(
        shader_mm,
        "config.metal4SharedShaderLibraryRepositoryEnabled",
        SHADER_MM,
    )
    require(
        shader_mm,
        "config.metal4SharedShaderLibraryResidentLimit",
        SHADER_MM,
    )
    assert "getenv(" not in shader_mm[: shader_mm.index("#if MVK_XCODE_26")]
    require(shader_h, "static MVKShaderLibraryRepository* create(MVKDevice* device);", SHADER_H)
    require(shader_mm, "getSharedShaderLibraryTrimHighWater", SHADER_MM)
    require(shader_h, "size_t _residentTrimHighWater = 0;", SHADER_H)
    assert "Metal 4 shared shader-library repository enabled:" not in shader_mm
    assert "Metal 4 shared shader-library resident limit:" not in shader_mm
    assert "Metal 4 shared shader-library repository summary:" not in shader_mm

    require(private_api_h, "MVKPipelineCacheMemoryStatistics", PRIVATE_API_H)
    require(private_api_h, "MVKMetal4ShaderLibraryRepositoryStatistics", PRIVATE_API_H)
    require(private_api_h, "vkGetPipelineCacheMemoryStatisticsMVK", PRIVATE_API_H)
    require(private_api_h, "vkGetMetal4ShaderLibraryRepositoryStatisticsMVK", PRIVATE_API_H)
    assert private_api_h.index("snapshotSkippedShaderLibraryCount") < private_api_h.index("costAwareCandidateCount")
    assert private_api_h.index("costAwareCandidateCount") < private_api_h.index("unknownRehydrateCostCandidateCount")
    require(shader_h, "tryGetMemorySnapshot", SHADER_H)
    require(shader_h, "getMemoryStatistics(MVKMetal4ShaderLibraryRepositoryStatistics", SHADER_H)
    require(shader_mm, "rehydrateTotalNanoseconds", SHADER_MM)
    require(shader_mm, "costAwareCandidateCount", SHADER_MM)
    require(shader_mm, "rehydrateProtectionSkipCount", SHADER_MM)
    require(shader_mm, "currentUseSequence + 256", SHADER_MM)
    require(shader_mm, "kUnknownRehydrateCostNanoseconds", SHADER_MM)
    require(shader_mm, "evictedUncompressedMSLBytes", SHADER_MM)
    require(shader_mm, "deviceCurrentAllocatedBytes", SHADER_MM)

    # A repository lookup must happen before either source compilation or
    # persisted-MSL construction, otherwise an already-published winner would
    # still incur duplicate work.
    conversion_lookup = shader_mm.index(
        "MVKShaderLibraryCache::addShaderLibrary(const SPIRVToMSLConversionConfiguration* pShaderConfig,\n"
        "\t\t\t\t\t\t\t\t\t\t\t\t\t\t  const SPIRVToMSLConversionResult& conversionResult)"
    )
    conversion_ctor = shader_mm.index("new MVKShaderLibrary(_owner, conversionResult)", conversion_lookup)
    conversion_acquire = shader_mm.index("_repository->acquire(_shaderModuleKey, &alignedConfig)", conversion_lookup)
    assert conversion_acquire < conversion_ctor

    import_lookup = shader_mm.index("const MVKCompressor<std::string> compressedMSL)")
    import_ctor = shader_mm.index("new MVKShaderLibrary(_owner, resultInfo, compressedMSL)", import_lookup)
    import_acquire = shader_mm.index("_repository->acquire(_shaderModuleKey, &alignedConfig)", import_lookup)
    assert import_acquire < import_ctor

    # Initial compile/import errors are attributed to the creating cache. Once
    # published, the shared physical object is rebound to the stable device
    # owner; later rehydrate failure must not poison the VkDevice.
    require(shader_h, "class MVKShaderLibraryRepository : public MVKVulkanAPIDeviceObject", SHADER_H)
    require(shader_mm, "candidate->_owner = this;", SHADER_MM)
    require(shader_mm, "if (!_repository) { _owner->setConfigurationResult(error); }", SHADER_MM)

    # A failed MTLLibrary candidate must never become the canonical object.
    # Null results must not be inserted into a logical cache view either.
    require(
        shader_mm,
        "if (!result && candidate && candidate->isResident())",
        SHADER_MM,
    )
    assert shader_mm.count(
        "if (shLib) { _shaderLibraries.emplace_back(alignedConfig, shLib); }"
    ) == 2
    require(
        shader_mm,
        "if (shared) { _shaderLibraries.emplace_back(alignedConfig, shared); }",
        SHADER_MM,
    )
    require(shader_mm, "wasAdded = shLib != nullptr;", SHADER_MM)
    assert shader_mm.count("priorConfigurationResult == VK_SUCCESS") == 3
    assert shader_mm.count("_owner->clearConfigurationResult();") == 3

    # The race loser must be released after the repository lock is dropped.
    # A failed candidate is released too, but is not counted as a race loser.
    require(shader_mm, "if (rejectedCandidate) {", SHADER_MM)
    assert "isTrackedLocked" not in shader_h
    assert "isTrackedLocked" not in shader_mm
    require(
        shader_mm,
        "_repositoryTracked.store(true, memory_order_release)",
        SHADER_MM,
    )
    require(
        shader_mm,
        "_repositoryTracked.store(false, memory_order_release)",
        SHADER_MM,
    )
    require(
        shader_mm,
        "if (result && !adoptedCandidatePayload)",
        SHADER_MM,
    )
    require(shader_mm, "rejectedCandidate->release();", SHADER_MM)
    require(
        shader_mm,
        "bool MVKShaderLibrary::tryAdoptResidentPayload(MVKShaderLibrary* candidate)",
        SHADER_MM,
    )
    adoption_start = shader_mm.index("bool MVKShaderLibrary::tryAdoptResidentPayload")
    adoption_body = shader_mm[adoption_start : adoption_start + 1900]
    assert "_mtlLibrary = candidate->_mtlLibrary;" in adoption_body
    assert "candidate->_mtlLibrary = nil;" in adoption_body
    assert "libraryBecameResident(this, false, 0)" in adoption_body
    acquire_start_for_adoption = shader_mm.index("MVKShaderLibrary* MVKShaderLibraryRepository::acquire(")
    acquire_body_for_adoption = shader_mm[acquire_start_for_adoption : acquire_start_for_adoption + 6500]
    assert "shouldAdoptCandidatePayload" in acquire_body_for_adoption
    assert "tryAdoptResidentPayload(candidate)" in acquire_body_for_adoption
    assert "!adoptedCandidatePayload" in acquire_body_for_adoption

    # A missing logical membership is an invariant violation, not permission
    # to decrement the physical object's reference count.
    release_start = shader_mm.index("void MVKShaderLibraryRepository::release(")
    release_body = shader_mm[release_start : release_start + 2600]
    missing_guard = release_body.index("if (!found) {")
    missing_return = release_body.index("return;", missing_guard)
    membership_release = release_body.index("library->release();", missing_return)
    assert missing_guard < missing_return < membership_release

    # Cache destruction releases logical membership; it must not directly
    # destroy a shared physical library.
    destructor = shader_mm.index("MVKShaderLibraryCache::~MVKShaderLibraryCache()")
    destructor_body = shader_mm[destructor : destructor + 700]
    assert "_repository->release" in destructor_body
    assert "slPair.second->destroy()" not in destructor_body

    # Sharing introduces cross-cache concurrency. Resident eviction must use
    # a non-blocking library lock and release only the hot Metal payload.
    eviction = shader_mm.index("bool MVKShaderLibrary::tryEvictResident(")
    eviction_body = shader_mm[eviction : eviction + 1300]
    assert "try_to_lock" in eviction_body
    assert "_mtlLibrary = nil" in eviction_body
    assert "releasedVariants.swap(_specializationVariants)" in eviction_body
    assert "evictedUncompressedMSLBytes" in eviction_body
    require(
        shader_mm,
        "item.second->_compressedMSL._uncompressedSize",
        SHADER_MM,
    )
    assert "_compressedMSL.clear" not in eviction_body
    assert "_compressedMSL =" not in eviction_body
    assert "_repository->libraryBecameCold(" in eviction_body

    # Cold entries must refuse an implicit compile for FAIL_ON_COMPILE_REQUIRED,
    # while the normal path can rehydrate from compressed MSL.
    ensure = shader_mm.index("bool MVKShaderLibrary::ensureResidentLocked")
    ensure_body = shader_mm[ensure : ensure + 600]
    assert "if (!allowLibraryCompile) { return false; }" in ensure_body
    require(
        shader_mm,
        "if (!wasResident && allowLibraryCompile)",
        SHADER_MM,
    )
    assert "decompressMSL(msl)" in ensure_body
    assert "compileLibrary(msl)" in ensure_body
    require(
        shader_mm,
        "!pipeline->shouldFailOnPipelineCompileRequired())",
        SHADER_MM,
    )

    # Resident accounting must be published before the library lock is
    # released, so a concurrent trim cannot miss a newly rehydrated payload.
    function_start = shader_mm.index("MVKMTLFunction MVKShaderLibrary::getMTLFunction")
    function_body = shader_mm[function_start : function_start + 14000]
    resident_notify = function_body.index("libraryBecameResident(this, true, rehydrateNanoseconds)")
    access_unlock = function_body.index("accessLock.unlock();", resident_notify)
    metal_function_lookup = function_body.index("newFunctionWithName", access_unlock)
    local_release = function_body.index("[lib release];", metal_function_lookup)
    active_release = function_body.index("_activeUseCount.fetch_sub", local_release)
    trim_after_release = function_body.index("trimToResidentLimit(this)", active_release)
    assert resident_notify < access_unlock < metal_function_lookup < local_release < active_release < trim_after_release
    retain_before_unlock = function_body.index("[lib retain];")
    active_acquire = function_body.index("_activeUseCount.fetch_add", retain_before_unlock)
    assert retain_before_unlock < active_acquire < access_unlock
    assert "retainedLibraryContentKey = *libraryContentKey;" in function_body
    assert "VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT" in function_body
    assert "unique_lock<mutex> accessLock(_accessLock, defer_lock);" in function_body
    assert "if (_repository) { accessLock.lock(); }" in function_body

    # Macro-specialized MTLLibrary variants are compilation too. A warm base
    # library must not bypass FAIL_ON_COMPILE_REQUIRED when the requested
    # macro variant is absent.
    specialization_lookup = shader_mm.index(
        "auto entry = _specializationVariants.find(spec_list);"
    )
    specialization_ctor = shader_mm.index(
        "new MVKShaderLibrary(_owner, _shaderConversionResultInfo, _compressedMSL, &spec_list)",
        specialization_lookup,
    )
    specialization_guard = shader_mm.index(
        "if (!allowLibraryCompile) { return MVKMTLFunctionNull; }",
        specialization_lookup,
    )
    assert specialization_guard < specialization_ctor

    # Reclaim must be per physical entry. It snapshots candidates while the
    # repository is locked, then performs non-blocking Metal release outside
    # that lock, preserving independent logical cache views.
    trim = shader_mm.index("void MVKShaderLibraryRepository::trimToResidentLimit")
    trim_end = shader_mm.index(
        "\nMVKShaderLibrary* MVKShaderLibraryRepository::acquire(", trim
    )
    trim_body = shader_mm[trim:trim_end]
    assert "_residentTrimHighWater" in trim_body
    lock_end = trim_body.index("sort(candidates.begin()")
    evict_call = trim_body.index(
        "candidate.library->tryEvictResident(candidate.snapshot, currentUseSequence)"
    )
    assert lock_end < evict_call
    assert "tryGetEvictionSnapshot(snapshot)" in trim_body
    retained_collect = trim_body.index("retainedLibraries.push_back(library)")
    snapshot_call = trim_body.index("tryGetEvictionSnapshot(snapshot)")
    assert retained_collect < snapshot_call
    assert "rehydrateProtectedUntilSequence" in trim_body
    assert "kUnknownRehydrateCostNanoseconds" in trim_body
    assert "_costAwareCandidateCount" in trim_body
    eviction_start = shader_mm.index(
        "bool MVKShaderLibrary::tryEvictResident("
    )
    eviction_guard = shader_mm[eviction_start : eviction_start + 1400]
    assert "_activeUseCount.load(memory_order_acquire) != 0" in eviction_guard
    assert "getLastUseSequence() != snapshot.lastUseSequence" in eviction_guard
    assert "currentUseSequence < snapshot.rehydrateProtectedUntilSequence" in eviction_guard
    assert "library->retain()" in trim_body
    assert "for (MVKShaderLibrary* library : retainedLibraries) { library->release(); }" in trim_body
    acquire_start = shader_mm.index("MVKShaderLibrary* MVKShaderLibraryRepository::acquire(")
    acquire_end = shader_mm.index("void MVKShaderLibraryRepository::release(", acquire_start)
    acquire_body = shader_mm[acquire_start:acquire_end]
    assert "entry.library->touch();" not in acquire_body
    assert "candidate->touch();" not in acquire_body

    # Per-pipeline function descriptors are construction-only. Retaining them
    # until VkPipeline destruction would keep the shared MTLLibrary alive after
    # repository eviction. Release must happen before the constructor's first
    # post-build early return, and all function keys must be cleared too.
    pipeline_build = pipeline_mm.index("initMTLRenderPipelineState(pCreateInfo")
    pipeline_invalid_return = pipeline_mm.index(
        "if ( !_hasValidMTLPipelineStates ) { return; }", pipeline_build
    )
    descriptor_gate = pipeline_mm.index(
        "if (device->getShaderLibraryRepository())", pipeline_build
    )
    descriptor_release = pipeline_mm.index(
        "[_metal4VertexFunctionDescriptor release];", descriptor_gate
    )
    assert pipeline_build < descriptor_gate < descriptor_release < pipeline_invalid_return
    release_window = pipeline_mm[descriptor_release:pipeline_invalid_return]
    assert "_metal4VertexFunctionDescriptor = nil;" in release_window
    assert "_metal4FragmentFunctionDescriptor = nil;" in release_window
    assert "_metal4VertexFunctionKey.clear();" in release_window
    assert "_metal4FragmentFunctionKey.clear();" in release_window
    assert "_metal4VertexPointerFunctionKey.clear();" in release_window
    assert "_metal4FragmentPointerFunctionKey.clear();" in release_window

    # The fixed-size Metal 4 base cache may retain input descriptors only while
    # its asynchronous base compile is in flight. Once entry->pipeline is
    # published, specialization derives from that pipeline, so the repository
    # path must release both descriptor inputs before waking coalesced waiters.
    base_publish = pipeline_mm.index("entry->pipeline = basePipeline;")
    base_vertex_release = pipeline_mm.index(
        "[entry->vertexFunction release];", base_publish
    )
    base_fragment_release = pipeline_mm.index(
        "[entry->fragmentFunction release];", base_vertex_release
    )
    base_ready = pipeline_mm.index("entry->ready.notify_all();", base_fragment_release)
    base_release_window = pipeline_mm[base_publish:base_ready]
    assert base_publish < base_vertex_release < base_fragment_release < base_ready
    assert "if (impl->device->getShaderLibraryRepository())" in base_release_window
    assert "entry->vertexFunction = nil;" in base_release_window
    assert "entry->fragmentFunction = nil;" in base_release_window

    # Serialization must continue to use per-cache logical membership and the
    # retained cold metadata even when the physical MTLLibrary is not resident.
    require(
        pipeline_mm,
        "getCompressedMSL() { return _pSLCache->_shaderLibraries[_index].second->getCompressedMSL(); }",
        PIPELINE_MM,
    )

    # A completed Global-cache Pipeline can publish only the exact shader
    # libraries that contributed to it into one destination cache view. This
    # must be a logical-membership operation: never a whole-cache merge and
    # never a second native Pipeline creation.
    require(
        private_api_h,
        "PFN_vkAdoptPipelineCacheShaderLibrariesMVK",
        PRIVATE_API_H,
    )
    require(
        private_api_h,
        "PFN_vkBeginPipelineCacheShaderLibraryCaptureMVK",
        PRIVATE_API_H,
    )
    require(
        private_api_h,
        "PFN_vkCancelPipelineCacheShaderLibraryCaptureMVK",
        PRIVATE_API_H,
    )
    require(
        private_api_h,
        "PFN_vkDiscardPipelineCacheShaderLibrariesMVK",
        PRIVATE_API_H,
    )
    require(
        private_api_h,
        "vkAdoptPipelineCacheShaderLibrariesMVK(",
        PRIVATE_API_H,
    )
    require(private_api_h, "#define MVK_PRIVATE_API_VERSION   47", PRIVATE_API_H)
    require(private_api_h, "MVKPipelineCacheShaderLibraryCaptureToken", PRIVATE_API_H)
    require(private_api_h, "vkBeginPipelineCacheShaderLibraryCaptureMVK(", PRIVATE_API_H)
    require(private_api_h, "vkCancelPipelineCacheShaderLibraryCaptureMVK(", PRIVATE_API_H)
    require(private_api_h, "vkDiscardPipelineCacheShaderLibrariesMVK(", PRIVATE_API_H)
    require(private_api_h, "VkPipeline                                pipeline", PRIVATE_API_H)
    require(private_api_h, "VkPipelineCache                           destinationPipelineCache", PRIVATE_API_H)
    require(private_api_h, "uint32_t*                                 pAdoptedShaderLibraryCount", PRIVATE_API_H)

    require(
        api_mm,
        "MVK_PUBLIC_VULKAN_SYMBOL VkResult vkAdoptPipelineCacheShaderLibrariesMVK(",
        API_MM,
    )
    require(
        api_mm,
        "MVK_PUBLIC_VULKAN_SYMBOL VkResult vkBeginPipelineCacheShaderLibraryCaptureMVK(",
        API_MM,
    )
    require(
        api_mm,
        "MVK_PUBLIC_VULKAN_SYMBOL VkResult vkCancelPipelineCacheShaderLibraryCaptureMVK(",
        API_MM,
    )
    require(
        api_mm,
        "MVK_PUBLIC_VULKAN_SYMBOL VkResult vkDiscardPipelineCacheShaderLibrariesMVK(",
        API_MM,
    )
    api_adoption_start = api_mm.index(
        "MVK_PUBLIC_VULKAN_SYMBOL VkResult vkAdoptPipelineCacheShaderLibrariesMVK("
    )
    api_adoption_body = api_mm[api_adoption_start : api_adoption_start + 1800]
    assert "adoptShaderLibrariesInto" in api_adoption_body
    assert "destinationPipelineCache" in api_adoption_body

    require(pipeline_h, "struct MVKPipelineShaderLibraryContribution", PIPELINE_H)
    require(pipeline_h, "MVKShaderModuleKey shaderModuleKey", PIPELINE_H)
    require(
        pipeline_h,
        "SPIRVToMSLConversionConfiguration shaderConfig",
        PIPELINE_H,
    )
    require(pipeline_h, "MVKShaderLibrary* shaderLibrary", PIPELINE_H)
    require(pipeline_h, "recordShaderLibraryContribution(", PIPELINE_H)
    require(pipeline_h, "beginShaderLibraryContributionCapture(", PIPELINE_H)
    require(pipeline_h, "cancelShaderLibraryContributionCapture(", PIPELINE_H)
    require(pipeline_h, "shouldRecordShaderLibraryContributions()", PIPELINE_H)
    require(pipeline_h, "adoptShaderLibrariesInto(", PIPELINE_H)
    require(pipeline_h, "discardShaderLibraryContributions(", PIPELINE_H)
    require(pipeline_h, "_shaderLibraryContributions", PIPELINE_H)
    require(pipeline_h, "adoptShaderLibraryMembership(", PIPELINE_H)
    require(shader_h, "adoptShaderLibraryMembership(", SHADER_H)

    require(
        pipeline_mm,
        "pipeline->recordShaderLibraryContribution(",
        PIPELINE_MM,
    )
    record_start = pipeline_mm.index("pipeline->recordShaderLibraryContribution(")
    record_body = pipeline_mm[record_start - 120 : record_start + 500]
    assert "shaderModule->getKey()" in record_body
    assert "*pContext" in record_body
    assert "shLib" in record_body
    assert "shouldRecordShaderLibraryContributions()" in record_body

    capture_state_start = pipeline_mm.index(
        "struct MVKPipelineShaderLibraryCaptureState"
    )
    capture_state_body = pipeline_mm[capture_state_start : capture_state_start + 2600]
    assert "thread_local" in capture_state_body
    assert "sourcePipelineCache" in capture_state_body
    assert "generation" in capture_state_body
    assert "active" in capture_state_body

    begin_capture_start = pipeline_mm.index(
        "VkResult MVKPipeline::beginShaderLibraryContributionCapture("
    )
    begin_capture_end = pipeline_mm.index(
        "VkResult MVKPipeline::cancelShaderLibraryContributionCapture(",
        begin_capture_start,
    )
    begin_capture_body = pipeline_mm[begin_capture_start:begin_capture_end]
    assert "sourcePipelineCache->getDevice()->getShaderLibraryRepository()" in begin_capture_body
    assert "VK_ERROR_FEATURE_NOT_PRESENT" in begin_capture_body
    assert begin_capture_body.index("VK_ERROR_FEATURE_NOT_PRESENT") < begin_capture_body.index(
        "_mvkPipelineShaderLibraryCaptureState ="
    )

    pipeline_ctor_start = pipeline_mm.index("MVKPipeline::MVKPipeline(")
    pipeline_ctor_end = pipeline_mm.index("MVKPipeline::~MVKPipeline()", pipeline_ctor_start)
    pipeline_ctor_body = pipeline_mm[pipeline_ctor_start:pipeline_ctor_end]
    assert "consumeShaderLibraryContributionCapture(pipelineCache)" in pipeline_ctor_body

    pipeline_adoption_start = pipeline_mm.index(
        "VkResult MVKPipeline::adoptShaderLibrariesInto("
    )
    pipeline_adoption_body = pipeline_mm[
        pipeline_adoption_start : pipeline_adoption_start + 2200
    ]
    assert "_shaderLibraryContributions" in pipeline_adoption_body
    assert "destinationPipelineCache->adoptShaderLibraryMembership(" in pipeline_adoption_body
    assert "pAdoptedShaderLibraryCount" in pipeline_adoption_body
    assert "swap(_shaderLibraryContributions)" in pipeline_adoption_body
    assert "releaseShaderLibraryContributions" in pipeline_adoption_body
    assert "getShaderLibraryRepository()" in pipeline_adoption_body
    assert "VK_ERROR_FEATURE_NOT_PRESENT" in pipeline_adoption_body
    assert "mergePipelineCaches" not in pipeline_adoption_body
    assert "createPipelines" not in pipeline_adoption_body
    assert "vkCreateGraphicsPipelines" not in pipeline_adoption_body
    assert "vkCreateComputePipelines" not in pipeline_adoption_body

    record_method_start = pipeline_mm.index(
        "void MVKPipeline::recordShaderLibraryContribution("
    )
    record_method_end = pipeline_mm.index(
        "VkResult MVKPipeline::adoptShaderLibrariesInto(", record_method_start
    )
    record_method_body = pipeline_mm[record_method_start:record_method_end]
    assert "shaderLibrary->retain();" in record_method_body
    assert record_method_body.index(
        "_shaderLibraryContributions.push_back("
    ) < record_method_body.index("shaderLibrary->retain();")

    pipeline_destructor_start = pipeline_mm.index("MVKPipeline::~MVKPipeline()")
    pipeline_destructor_end = pipeline_mm.index(
        "void MVKPipeline::recordShaderLibraryContribution(", pipeline_destructor_start
    )
    pipeline_destructor_body = pipeline_mm[
        pipeline_destructor_start:pipeline_destructor_end
    ]
    assert "releaseShaderLibraryContributions" in pipeline_destructor_body

    discard_start = pipeline_mm.index(
        "void MVKPipeline::discardShaderLibraryContributions("
    )
    discard_body = pipeline_mm[discard_start : discard_start + 900]
    assert "swap(_shaderLibraryContributions)" in discard_body
    assert "releaseShaderLibraryContributions" in discard_body

    release_start = pipeline_mm.index(
        "void MVKPipeline::releaseShaderLibraryContributions("
    )
    release_body = pipeline_mm[release_start : release_start + 800]
    assert "contribution.shaderLibrary->release();" in release_body
    assert "contributions.clear();" in release_body

    cache_adoption_start = pipeline_mm.index(
        "bool MVKPipelineCache::adoptShaderLibraryMembership("
    )
    cache_adoption_body = pipeline_mm[cache_adoption_start : cache_adoption_start + 1600]
    assert "getShaderLibraryCache(shaderModuleKey)" in cache_adoption_body
    assert "adoptShaderLibraryMembership(" in cache_adoption_body
    assert "markDirty();" in cache_adoption_body
    assert "mergePipelineCaches" not in cache_adoption_body
    assert "createPipelines" not in cache_adoption_body

    shader_cache_adoption_start = shader_mm.index(
        "bool MVKShaderLibraryCache::adoptShaderLibraryMembership("
    )
    shader_cache_adoption_body = shader_mm[
        shader_cache_adoption_start : shader_cache_adoption_start + 1400
    ]
    assert "if (!_repository) { return false; }" in shader_cache_adoption_body
    assert "new MVKShaderLibrary(*shaderLibrary)" not in shader_cache_adoption_body


def main() -> None:
    test_reference_and_residency_model()
    test_exact_pipeline_contribution_adoption_model()
    test_one_shot_capture_model()
    test_source_policy()
    print("metal4 shared shader-library repository policy: PASS")


if __name__ == "__main__":
    main()
