# IR static rasterizer-discard vertex producers

## Problem and supported cause

The XC3 DLC comparison showed missing player, companion and enemy bodies under IR while combat, terrain and UI continued. The same opening under MSL displayed those bodies. The IR run recorded 42 background pipeline failures with rasterizer-discard rejection. This identifies a missing legal pipeline class; linking a particular game buffer write to an actor still requires the saved-scene intervention below.

Vulkan rasterizer discard suppresses rasterization, but vertex execution and storage writes must still happen. The previous IR capability policy rejected every such pipeline. MeloNX can mark the whole ShaderCollection failed after a background graphics-pipeline error, so this is not merely a harmless unused variant.

An independent producer writes three known vertices into a sentinel storage buffer; a later regular draw consumes those bytes and must generate 256 red pixels. The supplied fragment guard must never execute. Direct and indexed draws exercise nonzero vertex origins.

The unchanged native baseline passed all four MSL controls and rejected all four IR producer pipelines with VK_ERROR_FEATURE_NOT_PRESENT. Simply permitting discard exposed the next boundary: Metal rejected the converted vertex return structure because a pipeline with rasterization disabled requires a void vertex function. That intermediate failure is retained; it is not a successful fix.

## Repair

- Preserve the pipeline constructor's static rasterizer-discard/cull decision, skip ignored fragment compilation and point-raster requirements, and avoid inspecting ignored multisample state.
- Select a separate no-raster vertex compilation variant. Keep vertex computations, resource accesses and writes; remove only the return value and raster-output metadata.
- Preserve MSC's original typed-pointer bitstream. LLVM 17 parsing alone upgrades pointers to opaque; reserializing that module is unsafe for Apple's bounds instrumentation. Append a new void function type and its pointer type after existing types, change only entry references/returns/output metadata, and keep other type and instruction records unchanged.
- Validate the rewritten module, rebuild the metallib payload ranges/hash/size, and retain the verified MSC AIR/language version gates.
- Extend ABI 9 with an additive request option and matching result annotation without changing structure sizes. A new native with an old compiler explicitly rejects the absent no-raster variant, with no MSL fallback. The annotation does not require a runtime root buffer.
- The existing cache key already includes runtime options and compiler dependency identity, so ordinary and no-raster variants cannot collide. Residency limits, default route, recovery policy and stage metadata ownership are unchanged.

## Validation so far

- CPU: original converted producer and positionless producer retain every non-return instruction; entry returns void and output metadata is empty. Truncated/invalid container ranges are rejected. The adapter also accepts a real MSC iOS 17 AIR 2.6 artifact produced on the host; this is artifact validation, not iOS GPU execution.
- GPU initial candidate: four MSL controls plus four IR cases pass all 16 buffer rows (including untouched guard/sentinels) and all 256 consumer pixels. Metal validation enabled. Independent CPU expected bytes; no game shader/hash predicates.
- Expanded GPU matrix: 25/25 pass, including omitted Position, both fragment-stage forms, direct/indexed draws, cold/disk restoration and explicit old-plugin rejection. Eight MSL controls, eight cold IR and eight restored IR executions have exact buffer and pixel output. The old-plugin case is an expected failure, not a pixel pass.
- Neighbor GPU fixtures: runtime 24/24 and point/topology/persistence 14/14 pass.
- Frozen DLC save reload verified. The old IR control loads the field with the objective at 322m and no movement, but player/companion bodies are absent. It records 22 background pipeline failures and 22 discard rejections, then exits normally with code 0. The repaired IR counterpart loads the same frozen progress, objective at 322m without movement: Matthew and A are visible. It records zero background pipeline failures and zero discard rejections and exits normally with code 0. Menu and Loading text remain missing; that is a separate unresolved rendering divergence.

## Scope and limits

Static discard is the repair target. This does not add tessellation, mesh, multiview or specialization support, change advertised Vulkan features, move IR to Metal 4 pipeline compilation, or establish full Vulkan conformance. Host pixel validation and a short saved-scene regression cannot replace iPhone/iPad acceptance. Original MeloNX #285 and native #24 remain unchanged and unmerged.

Private artifacts, game data, saves, source shader dumps and resource mappings remain outside Git. Baseline and failed intermediate results are retained locally alongside candidate identities and actual loaded native/compiler hashes.
