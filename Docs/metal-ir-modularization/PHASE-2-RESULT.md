# Phase 2: immutable process shader route

Native is based on MoltenVK #29 (`9471ce37`); product is based on MeloNX #285 (`f6ad94bf`). No source PR head/base or default changed.

The authoritative native snapshot freezes before MVKInstance construction/physical enumeration. C# consumes it through a single process Lazy; Swift still applies the request and probes availability, preserving its existing launch rejection. Requested, availability and selected are distinct diagnostics. MSL does not load the IR plugin merely to manufacture an availability result: its unprobed availability is explicitly unknown.

Explicit unavailable IR selects Unavailable and keeps the old IR error path; it never enables an automatic MSL fallback. Swift still refuses to launch this case. This conservative resolution preserves the original behavior rather than interpreting selected=false as permission to choose MSL. Legacy native without the new snapshot export remains supported with explicit unverified coverage until phase6 dependency alignment.

IR environment access is centralized, retaining capture scopes: route process-immutable; plugin/worker static at their former first-use consumers; mutable diagnostics storage initializes once; device/title cache at device state creation; stage/strict-math options per compile. Cache64/8MiB, worker2, MSL512 and default MSL route unchanged.

Validation: native Release build and product Vulkan Release build pass; Swift parse passes; 19 filtered product tests pass. The five queued native cases pass (MSL, IR, missing plugin, wrong ABI, Swift prewarm/setting-over-env); each verifies identical snapshot before/after physical enumeration and exact loaded native identity. Additional 24-case execution/cache regression is queued separately and its result will be recorded in the final report. No device or game acceptance claim.

The new-worktree build initially lacked NuGet assets and the existing JIT support dylib. Restore/build and the existing native Makefile resolved these prerequisites; no production change was made for them. Raw failures/results live in ~/MeloNX-debug/ir-memory-plan/phase2-evidence/.

Commits: native refactor e869c3c5; native tests2742f8b8/8ed39501. Product refactor84f4e4b814; tests7ff4011659. Stage3 follows without waiting for review. No unresolved correctness blocker; #332/#333 remain assigned to their later phases.
