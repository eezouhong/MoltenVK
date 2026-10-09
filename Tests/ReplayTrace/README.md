# Replay diagnostics compile gate

`MVK_REPLAY_TRACE` defaults to 0 in all ordinary builds. CMake exposes `-DMVK_REPLAY_TRACE=ON` for host diagnostics; Xcode callers may add `MVK_REPLAY_TRACE=1` to their explicit preprocessor definitions. Runtime flags cannot enable a disabled build.

The disabled headers provide empty inline scopes and unavailable getters. Queue/command-encoder diagnostic storage, Objective-C GPU callbacks, and the four replay exports are compiled out. `disabled_trace_test.cpp` can be compiled at `-O2 -DMVK_REPLAY_TRACE=0`; its object must reference no clock, environment lookup, TLS initialization, allocation, or static guard. A disabled build must export no `vkGetReplay*MVK` or `vkFinishReplayFrameMVK`; an enabled one exports all four.

Pure tests build with `-std=c++17 -pthread -DMVK_REPLAY_TRACE=1` and the GPUObjects include directory: binding/descriptor clocks and calibration, frame interval unions and bounded late-completion handling, stage-slot ownership, and draw-work overflow/unknown-input handling.

Actual graphics runs must enter the canonical Debug Tool queue. Enabled runtime flags select coarse frame/submission timing, optional GPU stage sampling, and optional binding groups. Keep paired instrumentation identical; report calibration and availability. These instrumented CPU/GPU costs are not an uninstrumented product FPS result.

Counters describe command encoding/API input counts, not actual hardware shader invocations. Generic indirect commands write `allIndirectDraws`/`allIndirectDispatches` in their encode entry points; synthetic fan delegation does not count twice. IR-only runtime fields are wired by the dependent IR integration and must be unavailable until a required producer/path fixture validates them. Zero alone is no correctness or zero-copy proof.

`trace_mesh_probe` requires mesh-shader support and verifies GPU output from two indirect mesh commands. The enabled coarse frame must report `allIndirectDraws=2` and `allIndirectDispatches=0`; the disabled binary must render the same output and expose no frame getter. A missing mesh feature is unavailable coverage, not a successful zero. Run through the same Debug Tool queue as `trace_probe`.
