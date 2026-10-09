# Triangle fan system-value replay

Build with explicit Vulkan headers and the tested MoltenVK dylib. Execute `fan_probe direct|indirect FIRST REQUESTED EXPECTED fan.vert.spv fan.frag.spv` through the host Debug Tool graphics queue with Metal API and shader validation.

For Vulkan system-value correctness use FIRST 0, 5, 130000 and REQUESTED = EXPECTED = 3 or 2048. Output records every invoked vertex and two instances, including BaseVertex/BaseInstance, against the original command; guard rows must remain untouched. FIRST 130000 with COUNT 2048 crosses the old indirect absolute-index allocation bound.

The indirect converter already reserves only 131072 source fan vertices. A separate safety fixture REQUESTED 131073 / EXPECTED 131072 verifies the capacity guard for writes and indexed fetches; it documents truncation above that existing internal limit and is not a claim of Vulkan conformance for larger indirect fans. Direct draws retain their original count. Multi-draw overlap is covered separately by the conversion kernel single writer with a shared triangle-list range.

Run MSL against this independent fix. Run the same executable with IR against the integration branch after applying this fix; this standalone PR adds no IR compiler dependency. Old MSL nonzero FIRST is an expected oracle failure. An old large indirect case must be validation-protected and its failure retained, never labelled passing.

Nonindexed indirect fans generate the final triangle list once, with provoking-vertex ordering preserved, and bypass the generic indexed-fan re-expansion. This bounds both the source range and expanded triangle output; clamping only the identity writer would still overflow that second conversion for a large valid count. Generic indexed fans are outside this targeted change.
