# Phase 3: product backend services, neutral paths and independent IR capacity

Product stacked on #334; native telemetry companion stacked on #30. The original #285/#24/#27/#328 heads and bases remain unchanged.

26 files were first moved byte-for-byte into Shared/Msl/MetalIR. The 18 existing MSL internal files remain byte-identical after all changes. Services wrap existing sessions; renderer construction, program configuration, idle relief and telemetry delegate to the selected service. Shutdown still drains users before closing the same MSL session.

GAL now carries BackendCacheRoot plus neutral preparation eligibility; GPU has no IR/MSL route names. Backend services retain the exact msl-v1/metal-ir-v1 disk paths, lowercase title IDs and the old no-title distinction. Legacy scheduling eligibility is preserved, not inferred from the new root alone. The remaining coupling is registered separately as #335 and not changed here.

Fix #332: IR capacity reads only MELONX_METAL_IR_CACHE_MAX_GB through its own parser; default2GiB,0 unlimited,1–5GiB. New settings expose IR capacity and disable MSL controls while IR is requested, retaining the saved MSL preference. Codable migration copies the old explicitly stored effective cap only if MSL was enabled; otherwise IR remains2GiB. Explicit new IR values win. Migration does not touch cache files or shrink old data. Existing user-initiated cache clearing already includes metal-ir-v1; no duplicate was added.

The already-reviewed #27/#328 version1/37-field schema and explicit availability are reused. Native companion preserves phase2 configuration access when resolving the single cherry-pick context overlap. Basic native counters enable independently of replay or compiled performance sampling; no replay timer is enabled. This is the requested measurement support, not a new recycling policy.

Validation: native and Vulkan Release builds pass; no-perf/no-test Vulkan build passes. 402 filtered product tests pass,7 existing conditional cases skipped. CPU resident-cache test and MSL UI wiring test pass; Swift parse plus8 executed migration cases pass. Queued trace-OFF GPU fixture first reads counters disabled, enables the explicit API, then compiles2/restores0 cold and compiles0/restores2 warm. Both37-field snapshots have flags7, live/retained libraries2; budgets remain64/8MiB. Actual native/plugin identity and GPU validation are checked. Pure-move/internal-byte identity evidence is retained.

Initial test failures were outdated reflected renderer fields/source paths and source-contract strings after the deliberate refactor; fixtures now inject the same original MSL session through the new adapter, retaining concurrency/drain assertions. Raw failed runs remain in phase3-evidence. No device, game, FPS, physical memory-saving or warning-reclaim claim.

Commits use move/refactor/fix/test/docs classification. Only the #332 fix changes user capacity/control behavior; counters adopt previously reviewed measurement semantics. Defaults512/64/workers/math/route are unchanged. #333 capability advertising assessment is next; #331 active warning cleanup is excluded. Phase4 proceeds without waiting for review.
