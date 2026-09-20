#!/usr/bin/env python3

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    target = ROOT / path
    if not target.is_file():
        raise AssertionError(f"missing source file: {path}")
    return target.read_text(encoding="utf-8")


def require(source: str, pattern: str, message: str) -> None:
    if not re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


def reject(source: str, pattern: str, message: str) -> None:
    if re.search(pattern, source, re.MULTILINE | re.DOTALL):
        raise AssertionError(message)


def main() -> int:
    private_api = read("MoltenVK/MoltenVK/API/mvk_private_api.h")
    pipeline_h = read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h")
    pipeline_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm")
    shader_h = read("MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.h")
    shader_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm")
    shader_work_h = read("MoltenVK/MoltenVK/GPUObjects/MVKShaderLibraryWork.h")
    library_trace = pipeline_mm[
        pipeline_mm.index("static void recordMetal4DiagnosticLibraryTrace") :
        pipeline_mm.index("static MVKMetal4CompilerWorkStatistics* beginMetal4DiagnosticBaseLookup")
    ]
    work_context = pipeline_mm[
        pipeline_mm.index("struct MVKMetal4DiagnosticWorkContext") :
        pipeline_mm.index("static thread_local MVKMetal4DiagnosticWorkContext")
    ]

    for field in (
        "libraryTraceCount",
        "libraryTraceOverflowCount",
        "library0ContentFingerprint",
        "library0SourceBytes",
        "library0TaskNanoseconds",
        "library1ContentFingerprint",
        "library1SourceBytes",
        "library1TaskNanoseconds",
        "baseKeyFingerprint",
        "baseLookupCount",
        "baseMemoryHitCount",
        "baseCompileCount",
        "baseCompileNanoseconds",
        "baseCoalescedWaitCount",
        "baseCoalescedWaitNanoseconds",
        "specializationCount",
        "specializationNanoseconds",
        "functionTraceCount",
        "functionTraceOverflowCount",
        "function0ContentFingerprint",
        "function0AccessWaitNanoseconds",
        "function0RehydrateNanoseconds",
        "function0DeviceLockWaitNanoseconds",
        "function0LookupNanoseconds",
        "function0SpecializationNanoseconds",
        "function1ContentFingerprint",
        "function1AccessWaitNanoseconds",
        "function1RehydrateNanoseconds",
        "function1DeviceLockWaitNanoseconds",
        "function1LookupNanoseconds",
        "function1SpecializationNanoseconds",
        "shaderWorkCount",
        "shaderWorkTotalNanoseconds",
        "shaderWorkLookupNanoseconds",
        "shaderWorkGateNanoseconds",
        "shaderWorkRecheckNanoseconds",
        "shaderWorkBuildNanoseconds",
        "shaderWorkReadyHitCount",
        "shaderWorkRecheckHitCount",
        "shaderWorkBuildCount",
        "shaderWorkTraceOverflowCount",
        "shader0ModuleHash",
        "shader0ModuleBytes",
        "shader0Stage",
        "shader0UsedInputCount",
        "shader0UsedOutputCount",
        "shader0UsedResourceCount",
        "shader0DiscreteSetCount",
        "shader0DynamicBufferCount",
        "shader1ModuleHash",
        "shader1ModuleBytes",
        "shader1Stage",
        "shader1UsedInputCount",
        "shader1UsedOutputCount",
        "shader1UsedResourceCount",
        "shader1DiscreteSetCount",
        "shader1DynamicBufferCount",
    ):
        require(private_api, rf"\b{field}\b", f"missing ABI field: {field}")

    require(
        pipeline_mm,
        r"recordMetal4DiagnosticLibraryTrace.*?getMetal4DiagnosticWork.*?"
        r"libraryTraceCount\+\+.*?traceIndex\s*==\s*0.*?traceIndex\s*==\s*1.*?"
        r"libraryTraceOverflowCount\+\+",
        "library trace must remain fixed at two entries plus overflow",
    )
    reject(
        library_trace,
        r"reportMessage",
        "native slow-chain attribution must not emit per-task text logs",
    )
    require(
        pipeline_mm,
        r"recordMetal4DiagnosticLibraryTrace\s*\(.*?contentKey.*?sourceBytes.*?taskDuration",
        "library completion must publish canonical content identity and timing",
    )
    require(
        shader_mm,
        r"makeMetal4LibraryContentKey.*?_metal4LibraryContentKey.*?"
        r"newMTLLibrary\s*\(.*?_metal4LibraryContentKey.*?msl\.size",
        "the existing SHA-256 library content key must reach the compiler trace",
    )
    require(
        pipeline_h + shader_h,
        r"newMTLLibrary\s*\(.*?contentKey.*?sourceBytes",
        "compiler declarations must carry bounded library identity",
    )
    require(
        pipeline_mm,
        r"beginMetal4DiagnosticBaseLookup.*?baseLookupCount\+\+.*?"
        r"baseKeyFingerprint",
        "flexible base lookup identity is missing",
    )
    require(
        pipeline_mm,
        r"recordMetal4DiagnosticBaseMemoryHit\s*\(.*?impl->baseHits\+\+",
        "resident base hits must be attributed to the active work scope",
    )
    require(
        pipeline_mm,
        r"recordMetal4DiagnosticBaseCompile\s*\(.*?compileDuration",
        "base-owner compilation duration is missing",
    )
    require(
        pipeline_mm,
        r"coalescedWaitStart.*?recordMetal4DiagnosticBaseCoalescedWait",
        "coalesced-base waits are missing",
    )
    require(
        pipeline_mm,
        r"specializationDuration.*?recordMetal4DiagnosticSpecialization",
        "final specialization duration is missing",
    )
    require(
        pipeline_mm,
        r"recordMetal4DiagnosticFunctionTrace.*?functionTraceCount\+\+.*?"
        r"traceIndex\s*==\s*0.*?traceIndex\s*==\s*1.*?"
        r"functionTraceOverflowCount\+\+",
        "shader-function trace must remain fixed at two entries plus overflow",
    )
    require(
        shader_mm,
        r"isDiagnosticWorkActive.*?accessWaitStart.*?_accessLock.*?"
        r"deviceLockWaitStart.*?functionLookupStart.*?"
        r"functionSpecializationStart.*?recordShaderFunctionTrace",
        "shader-library wait, rehydrate, function lookup, and specialization chain is incomplete",
    )
    require(
        shader_work_h + shader_h + shader_mm,
        r"perCallTiming.*?workTiming.*?recordShaderLibraryWorkTrace.*?"
        r"gateNs.*?recheckNs.*?buildNs",
        "module-local shader creation-gate timing is not joined to the work scope",
    )
    require(
        shader_mm + pipeline_mm,
        r"_shaderModuleKey\.codeHash.*?_shaderModuleKey\.codeSize.*?"
        r"entryPointStage.*?shaderInputs.*?shaderOutputs.*?resourceBindings.*?"
        r"shaderWorkTraceOverflowCount",
        "guest module, stage, and compact conversion-shape identity is incomplete",
    )
    require(
        pipeline_mm,
        r"stats\.available\s*=\s*requestId\s*!=\s*0\s*\?\s*VK_TRUE\s*:\s*VK_FALSE",
        "request id zero must keep detailed attribution disabled",
    )
    reject(
        work_context,
        r"(?:unordered_map|map<|vector<)",
        "same-thread trace state must remain fixed-size",
    )

    print("Metal 4 slow library trace source contract passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
