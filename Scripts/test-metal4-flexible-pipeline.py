#!/usr/bin/env python3

from pathlib import Path
import re
import sys


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
    device_h = read("MoltenVK/MoltenVK/GPUObjects/MVKDevice.h")
    device_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm")
    pipeline_h = read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.h")
    pipeline_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm")
    shader_h = read("MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.h")
    shader_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm")
    render_pass_mm = read("MoltenVK/MoltenVK/GPUObjects/MVKRenderPass.mm")
    command_buffer_mm = read("MoltenVK/MoltenVK/Commands/MVKCommandBuffer.mm")
    encoder_mm = read("MoltenVK/MoltenVK/Commands/MVKCommandEncoderState.mm")
    config_members = read("MoltenVK/MoltenVK/Utility/MVKConfigMembers.def")
    implementation = "\n".join(
        (
            device_h,
            device_mm,
            pipeline_h,
            pipeline_mm,
            shader_h,
            shader_mm,
            render_pass_mm,
            command_buffer_mm,
            encoder_mm,
        )
    )

    # Internal, default-off experiment. Do not extend the public MVKConfiguration ABI.
    require(
        implementation,
        r'MVK_CONFIG_METAL4_COMPILER["\s,]+0(?:\.0)?',
        "experimental environment gate is missing or is not default-off",
    )
    require(
        implementation,
        r'MVK_CONFIG_METAL4_FLEXIBLE_ASYNC["\s,]+0(?:\.0)?',
        "bounded async environment gate is missing or is not default-off",
    )
    reject(
        config_members,
        r"METAL4_FLEXIBLE",
        "experimental gate must not change the public MVKConfiguration ABI",
    )
    require(implementation, r"#if\s+MVK_XCODE_26", "Xcode 26 compile guard is missing")
    require(implementation, r"supportsMetal4", "Metal 4 GPU-family capability gate is missing")
    require(
        implementation,
        r"mvkOSVersionIsAtLeast\s*\(\s*26\.0\s*\)",
        "OS 26 runtime availability gate is missing",
    )
    require(
        implementation,
        r"newCompilerWithDescriptor\s*:",
        "device-owned public MTL4Compiler creation is missing",
    )

    # Device-owned lifetime, bounded cache, and fail-safe legacy fallback.
    require(device_h, r"MVKMetal4CompilerService\s*\*", "device compiler owner is missing")
    require(device_mm, r"delete\s+_metal4CompilerService", "device compiler cleanup is missing")
    require(pipeline_h, r"class\s+MVKMetal4CompilerService", "compiler service is missing")
    require(pipeline_h + pipeline_mm, r"cacheMax|cacheLimit|maxCache", "bounded base cache is missing")
    require(pipeline_h + pipeline_mm, r"lastUse|least|LRU|lru", "base-cache eviction tracking is missing")
    require(pipeline_mm, r"array<uint64_t,\s*4096>", "fixed distinct-key telemetry is missing")
    require(
        pipeline_mm,
        r"array<MVKMetal4BaseGhostEntry,\s*512>",
        "fixed recent-eviction telemetry is missing",
    )
    require(pipeline_mm, r"kMetal4TelemetryInterval\s*=\s*512", "telemetry is not low-frequency")
    require(
        pipeline_mm,
        r"kMetal4CardinalityBitmapWords\s*=\s*32",
        "base-key field cardinality storage is not fixed to 2048 bits per tracker",
    )
    require(
        pipeline_mm,
        r"struct\s+MVKMetal4BaseKeyFingerprints\s*\{.*?vertexFunction.*?fragmentFunction.*?"
        r"vertexLayout.*?fixedState",
        "base-key field families are incomplete",
    )
    require(
        pipeline_mm,
        r"struct\s+MVKMetal4ApproxCardinality\s*\{.*?array<uint64_t,\s*"
        r"kMetal4CardinalityBitmapWords>.*?record\s*\(.*?estimate\s*\(",
        "fixed-memory approximate cardinality tracker is missing",
    )
    require(
        pipeline_mm,
        r"recordMetal4BaseKeyCardinality\s*\(.*?vertexFunction.*?fragmentFunction.*?"
        r"shaderPair.*?vertexLayout.*?fixedState.*?withoutVertexFunction.*?"
        r"withoutFragmentFunction.*?withoutShaderPair.*?withoutVertexLayout.*?withoutFixedState",
        "base-key field and leave-one-out cardinality recording is incomplete",
    )
    require(
        pipeline_mm,
        r"struct\s+MVKMetal4BaseKeyFingerprints\s*\{.*?rasterSampleCount.*?"
        r"alphaToCoverage.*?alphaToOne.*?rasterization.*?vertexAmplification.*?"
        r"primitiveTopology.*?indirectCommandBuffers.*?shaderValidation.*?"
        r"withoutRasterSampleCount.*?withoutAlphaToCoverage.*?withoutAlphaToOne.*?"
        r"withoutRasterization.*?withoutVertexAmplification.*?withoutPrimitiveTopology.*?"
        r"withoutIndirectCommandBuffers.*?withoutShaderValidation",
        "individual fixed base-key fields and leave-one-out fingerprints are incomplete",
    )
    require(
        pipeline_mm,
        r"base_evictions.*?recent_rebuilds_after_eviction.*?distinct_base_keys.*?cache_high_water",
        "base-cache reuse telemetry is incomplete",
    )
    require(
        pipeline_mm,
        r"vertex_fn_distinct.*?fragment_fn_distinct.*?shader_pair_distinct.*?"
        r"stable_shader_pair_distinct.*?pointer_shader_pair_distinct.*?"
        r"vertex_layout_distinct.*?fixed_state_distinct.*?without_vertex_fn_distinct.*?"
        r"without_fragment_fn_distinct.*?without_shader_pair_distinct.*?"
        r"without_vertex_layout_distinct.*?without_fixed_state_distinct",
        "base-key field cardinality log fields are incomplete",
    )
    require(
        pipeline_mm,
        r"raster_samples_distinct.*?alpha_to_coverage_distinct.*?alpha_to_one_distinct.*?"
        r"rasterization_distinct.*?vertex_amplification_distinct.*?primitive_topology_distinct.*?"
        r"icb_support_distinct.*?shader_validation_distinct.*?without_raster_samples_distinct.*?"
        r"without_alpha_to_coverage_distinct.*?without_alpha_to_one_distinct.*?"
        r"without_rasterization_distinct.*?without_vertex_amplification_distinct.*?"
        r"without_primitive_topology_distinct.*?without_icb_support_distinct.*?"
        r"without_shader_validation_distinct",
        "individual fixed base-key cardinality log fields are incomplete",
    )
    require(
        pipeline_mm,
        r"baseCompileTotalNs.*?baseCompileMaxNs.*?specializationTotalNs.*?specializationMaxNs",
        "device-validation timing telemetry is missing",
    )
    for token in (
        "recordLegacyGraphicsCompile",
        "recordLegacyTessellationCompile",
        "recordLegacyComputeCompile",
    ):
        require(
            pipeline_h,
            rf"void\s+{token}\s*\(\s*uint64_t\s+durationNs\s*,\s*bool\s+success(?:\s*,\s*bool\s+fallback)?\s*\)",
            f"legacy compiler telemetry declaration is missing: {token}",
        )
    require(
        pipeline_mm,
        r"legacy_graphics_compiles.*?legacy_graphics_failures.*?legacy_graphics_total_ns.*?legacy_graphics_max_ns.*?"
        r"legacy_tessellation_compiles.*?legacy_tessellation_failures.*?legacy_tessellation_total_ns.*?legacy_tessellation_max_ns.*?"
        r"legacy_compute_compiles.*?legacy_compute_failures.*?legacy_compute_total_ns.*?legacy_compute_max_ns",
        "legacy graphics/tessellation/compute summary fields are incomplete",
    )
    require(
        pipeline_mm,
        r"MVKRenderPipelineCompiler.*?newMTLRenderPipelineState.*?"
        r"recordLegacy(?:Tessellation|Graphics)Compile",
        "legacy render compiler work is not timed and classified",
    )
    require(
        pipeline_mm,
        r"MVKGraphicsPipeline::getOrCompilePipeline\(MTLComputePipelineDescriptor.*?"
        r"recordLegacyTessellationCompile",
        "legacy tessellation compute stages are not timed and classified",
    )
    require(
        pipeline_mm,
        r"MVKComputePipeline::MVKComputePipeline.*?recordLegacyComputeCompile",
        "standalone legacy compute pipelines are not timed and classified",
    )
    require(
        pipeline_mm,
        r"newRenderPipelineStateBySpecializationWithDescriptor\s*:",
        "Metal 4 specialization call is missing",
    )
    require(
        pipeline_mm,
        r"newRenderPipelineStateWithDescriptor\s*:.*?completionHandler\s*:",
        "public asynchronous base compiler call is missing",
    )
    require(
        pipeline_mm,
        r"newRenderPipelineStateBySpecializationWithDescriptor\s*:.*?completionHandler\s*:",
        "public asynchronous specialization call is missing",
    )
    require(
        pipeline_mm,
        r"MVKMetal4CompilerTaskContext.*?waitFor\s*\(.*?return\s+pipeline",
        "bounded task context must finish before MoltenVK returns the pipeline",
    )
    if "waitUntilCompleted" in pipeline_mm:
        raise AssertionError("Metal 4 compiler tasks must not use unbounded waits")
    require(
        pipeline_mm,
        r"newMTLRenderPipelineState\s*\(\s*plDesc\s*\)",
        "legacy render-pipeline fallback was removed",
    )
    require(
        pipeline_mm,
        r"isTessellationPipeline\s*\(\)|mvkIsMultiview",
        "narrow tessellation/multiview eligibility gate is missing",
    )

    # Stable shader content and Vulkan specialization constants must flow into Metal 4.
    require(shader_h, r"getMTL4FunctionDescriptor", "Metal 4 function descriptor is not carried with MVKMTLFunction")
    require(shader_mm, r"MTL4LibraryFunctionDescriptor", "library function descriptor construction is missing")
    require(shader_mm, r"MTL4SpecializedFunctionDescriptor", "function-constant specialization descriptor is missing")
    require(shader_mm, r"constantValues\s*=", "function constant values are not preserved")
    reject(
        shader_mm,
        r"specializedName\s*=\s*mtlFunc\.name",
        "different function-constant variants must not reuse the same optional specialized name",
    )
    require(shader_h + shader_mm, r"getMTL4FunctionKey|mtl4FunctionKey", "function identity key is missing")
    for token in (
        "CommonCrypto/CommonDigest.h",
        "makeMetal4LibraryContentKey",
        "CC_SHA256_Init",
        "CC_SHA256_Update",
        "CC_SHA256_Final",
        "_metal4LibraryContentKey",
        "getMetal4LibraryContentKey",
        "makeMetal4PointerFunctionKey",
    ):
        require(shader_h + shader_mm, re.escape(token), f"stable shader content key is missing {token}")
    require(
        shader_mm,
        r"makeMetal4FunctionKey\s*\(\s*const\s+string&\s+libraryContentKey",
        "active function key does not use stable shader-library content",
    )
    reject(
        shader_mm,
        r"makeMetal4FunctionKey\s*\(\s*id<MTLLibrary>",
        "active function key still uses process-local MTLLibrary identity",
    )
    content_key_body = re.search(
        r"makeMetal4LibraryContentKey\s*\(.*?\n\}", shader_mm, re.MULTILINE | re.DOTALL
    )
    if not content_key_body:
        raise AssertionError("stable shader-library content-key implementation is missing")
    for token in (
        "fpFastMathFlags",
        "isPositionInvariant",
        "macroDefinitions",
        "contentKind == kMetal4SourceLibraryContent",
        "macro.first.name",
        "macro.first.isFloat",
        "macro.first.isSigned",
        "valueSize",
    ):
        require(
            content_key_body.group(0),
            re.escape(token),
            f"stable shader-library content key omits compile or macro input: {token}",
        )
    require(
        pipeline_mm,
        r"pointerShaderPairCardinality.*?stable_shader_pair_distinct.*?pointer_shader_pair_distinct",
        "stable-vs-pointer shader-pair telemetry is incomplete",
    )
    require(
        pipeline_mm,
        r"struct\s+BaseEntry\s*\{.*?MTL4FunctionDescriptor\s*\*\s*vertexFunction.*?"
        r"MTL4FunctionDescriptor\s*\*\s*fragmentFunction.*?"
        r"~BaseEntry\s*\(\s*\).*?\[\s*vertexFunction\s+release\s*\].*?"
        r"\[\s*fragmentFunction\s+release\s*\]",
        "base-cache entries must retain the function descriptors that own their MTLLibraries",
    )
    require(
        pipeline_mm,
        r"entry->vertexFunction\s*=\s*\[\s*vertexFunction\s+retain\s*\].*?"
        r"entry->fragmentFunction\s*=\s*\[\s*fragmentFunction\s+retain\s*\]",
        "base-cache misses do not retain the function descriptors that own their MTLLibraries",
    )

    # Attachment state is the only flexible state in this POC.
    for token in (
        "MTLPixelFormatUnspecialized",
        "MTL4BlendStateUnspecialized",
        "MTLColorWriteMaskUnspecialized",
        "MTLBlendFactorUnspecialized",
        "MTLBlendOperationUnspecialized",
    ):
        require(pipeline_mm, re.escape(token), f"missing unspecialized attachment field: {token}")
    for field in (
        "pixelFormat",
        "blendingState",
        "writeMask",
        "sourceRGBBlendFactor",
        "destinationRGBBlendFactor",
        "rgbBlendOperation",
        "sourceAlphaBlendFactor",
        "destinationAlphaBlendFactor",
        "alphaBlendOperation",
    ):
        require(
            pipeline_mm,
            rf"colorAttachment\s*\.{field}\s*=\s*legacyColor\s*\.{field}|"
            rf"colorAttachment\s*\.{field}\s*=\s*legacyColor\s*\.blendingEnabled",
            f"specialization does not assign {field}",
        )

    # Color mapping uses the existing legacy encoder, and legacy restart behavior remains present.
    require(
        pipeline_mm,
        r"MTL4LogicalToPhysicalColorAttachmentMappingStateInherited",
        "flexible base does not inherit the encoder color map",
    )
    require(pipeline_mm, r"MTLLogicalToPhysicalColorAttachmentMap", "pipeline color map is missing")
    require(render_pass_mm, r"supportColorAttachmentMapping\s*=\s*(?:YES|true)", "render pass does not opt into mapping")
    require(encoder_mm, r"setColorAttachmentMap\s*:", "legacy render encoder does not bind the pipeline map")
    require(
        encoder_mm,
        r"prepareHelperDraw\s*\([^)]*\)\s*\{.*?setColorAttachmentMap\s*:\s*nil",
        "helper draws do not clear inherited color-attachment mapping",
    )
    require(
        command_buffer_mm,
        r"updateColorAttachmentLocations\s*\([^)]*\)\s*\{.*?usesMetal4FlexiblePipeline\s*\(\).*?"
        r"identityLocations.*?updateColorAttachmentLocations\s*\(\s*effectiveColorAttLocs",
        "command-time attachment mappings are not normalized before a Metal 4 draw",
    )
    reject(
        command_buffer_mm,
        r"updateColorAttachmentLocations\s*\([^)]*\)\s*\{\s*if\s*\(\s*_mtlRenderEncoder\s*&&",
        "attachment mapping state must remain correct after a Metal encoder restart",
    )
    require(
        pipeline_mm,
        r"updateColorAttachmentLocations\s*\(",
        "legacy attachment-remap fallback was removed",
    )

    # This experiment must stay independent from the failed Archive and full Metal 4 command backend.
    for forbidden in (
        "MTL4Archive",
        "MTLBinaryArchive",
        "MTL4CommandQueue",
        "MTL4CommandBuffer",
        "MTL4RenderCommandEncoder",
        "MVK_USE_METAL_PRIVATE_API=1",
    ):
        reject(implementation, re.escape(forbidden), f"forbidden API entered the flexible-pipeline POC: {forbidden}")

    print("metal4 flexible pipeline source contract: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"metal4 flexible pipeline source contract: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
