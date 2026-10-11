# Pre-XC3 native integration review

This PR replaces the review dependency stack with one diff against RC6 `8ec88055b45b507ab31e5e364fb5d64b3848c2cc`. Its initial commit has the exact tree of native#33 (`d367b1f6`): strict IR from#24, the reviewed shared math policy, telemetry from#27, trace/tool extraction#29, immutable route#30, schema/services#31, backend/hooks/capabilities#32 and residency#33. The shared math header matches#28 byte-for-byte; its independent synthetic tests are included once.

## Scope boundary

Native#34–#37 are excluded: Layer/ViewportIndex parser support, no-raster vertex producer/void AIR adaptation, loader dispatch lifetime and sparse varying linkage. Their compiler patches and probes are absent. The complete phone Build375 and formal-ID IPA Build376 contain those later fixes and are not artifacts for this narrower branch. Original PRs remain open/unmerged; this PR itself merges nothing.

Generic compiler code, Metal binding and the original point support are part of#24 and remain included. This boundary is a chronological feature/modularization scope, not a title-specific workaround. Known unsupported inputs still fail explicitly without MSL fallback. Experimental IR remains off by default.

## REVIEW-341-STACK follow-ups

A1–A4 are separate commits after the unchanged consolidation:

- Field7 of the legacy basic compiler statistics means terminal shader-compilation, pipeline-preflight or descriptor-layout creation rejections. A failed layout is destroyed and returned as a null handle, so it cannot be reused for a later shader attempt. Layout creation failures are counted once. It is not a distinct-defect count. ABI9, the legacy7/8/9 array capacities and#27's version1 37-field memory/cache schema remain unchanged.
- Reasons now distinguish tessellation, mesh, rasterizer discard, multiview, vertex binding and sample shading. Support predicates/results and advertised features remain unchanged.
- Physical-device feature queries document why no logical-device backend exists yet.
- The stateless route/residency facade is constructed before logical-device helpers, after the existing lost-device early exit. Its accessor asserts initialization. Stage dispatch, encoded-memory ownership and teardown remain as before.

## Reviewer navigation

Review production under `MoltenVK/MoltenVK/GPUObjects` (route/backend/cache/compiler), `Commands` (binding/hooks) and `MetalIRCompiler/src` plus its seven Mesa patches. Public execution fixtures are in `Tests/MetalIRReplay`; CPU math/capability fixtures are in `Tests/ShaderMath`. Per-phase RESULT files retain historical evidence. `src/air/BitcodeWriter.cpp` is the pinned licensed writer used to preserve typed-pointer AIR; no private game shader or asset is included.

Budgets/policies remain IR64/8MiB, MSL512 and their distinct existing implementations. No new abstraction beyond the reviewed facade, eviction-policy transplant, UIKit warning cleanup, IR Metal4 flexible PSO or default change is included. The facade remains transitional; future MSL removal can delete its MSL lambdas/backend while keeping stage/resource contracts.

## Validation

Current focused validation is recorded in the final PR body after execution. Historical stage checks remain described in PHASE2–5 reports; those do not claim new device acceptance. Current compiler sources match the previously verified pre-XC3 host/iOS compiler, and integration pins reference this PR's exact source commit. iPad60+minute TOTK acceptance and remote CI remain pending (native Actions disabled; product billing/spending-limit). No device action or game run is needed to consolidate this source review.

## REVIEW-355-38 correction

The earlier A1 premise was incorrect: `vkCreateDescriptorSetLayout` destroys a rejected layout and returns an error/null handle, so no later compilation can reuse it. The follow-up restores layout rejection count1 and removes the optional suppression parameter. The earlier count0 result remains in private historical evidence and is superseded. The compiler framework remains the pre-XC3 variant without Mesa patch0008; its source/binary hash is unchanged by this native counter fix.
