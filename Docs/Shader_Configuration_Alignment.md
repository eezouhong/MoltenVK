# Shader configuration alignment: PR7 follow-up

Runtime tested: `6de1b98a86feb1a47217f5cf8ab83f84b9047386`. Neither PR138 nor PR7 is merged.

`alignWith` previously scanned every source binding for every destination binding.
Captured TOTK layouts contain 1,315 resource bindings (1,729,225 resource comparisons
per alignment). A call-local index now uses the original full `matches()` predicate
for equality. Coarse hashes are not result identity. Last-match-wins (including
last false), constexpr sampler semantics and missing entries are preserved. Small
vectors and source/destination self-alias retain the original loop. No new shader
cache, worker change, lifetime change, or draw policy is introduced.

Tests: `Scripts/test-shader-config-alignment.py --spirv-cross External/SPIRV-Cross
--sanitizer address --output result.json` compiles the actual configuration methods
and compares to the pinned original function body. 3,483 semantic/stress checks
plus 24 privately captured pairs pass under ASAN, UBSAN and TSAN. The 1,024-unique-
binding comparison-count gate fails on the old implementation (1,048,576 calls)
and passes on the candidate (1,024). Forced collisions and duplicate/self-alias
cases prevent approximate matching or unsafe same-index shortcuts.

Local same-input replay: 24 pairs, 96 calls per batch, seven alternating rounds.
Median original 202.802 ms; median candidate 4.675 ms. This is a function
microbenchmark, not game FPS or an entire native-chain speedup. No captured game
configuration files or temporary capture instrumentation are committed.

Two concentrated cache-off TOTK runs, ON and OFF, completed and exited 0.
Lookup/recheck sums were 0.196/0.199 seconds (earlier unmodified runs: about 5.3s).
Consumer pipeline blocking sums were 3.514/4.857 seconds. Both candidate runs had
zero pending skips and no recorded create failures/device loss/timeouts. Exact
pipeline task sets and Metal library times differ, so no whole-run causal speedup
percentage or phone acceptance is claimed. Visual inspection covers start/end
checkpoints, not every frame. The private host's original native/pin was restored.

Full macOS Release library build, real Vulkan GPU readback/cache-lifetime smoke,
and changed converter iPhoneOS arm64 compilation passed. Existing repository
policy and 13 production control-flow TSAN cases pass. The existing macOS CI job
now runs the semantic/comparison gate and uploads its evidence; CI results must
be checked against the final head. iPhoneOS translation-unit success is not a
linked iOS package/device test.

The initial fixture compared unspecified outer tail padding; final checks compare
all actual fields and keep the production internal memcmp semantics unchanged.
The attempted temporary function timing summary was not emitted at process exit;
no missing measurement is inferred. Captured-input replay and existing native
stage logs are the evidence used here.

Continue combined PR138+PR7 repeat-start acceptance before further optimization
or cache decoupling. The original phone 14/24 FPS split remains unaccepted.
