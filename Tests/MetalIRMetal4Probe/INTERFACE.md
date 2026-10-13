# Phase 0/1 interface for the archive task

Status: probe implemented; the following product interface is a proposal, not
implemented code. P0 ownership/defaults and archive ownership are unchanged.

## Existing service contract

- IR builds an `MTLLibrary` through `newLibraryWithData`; its artifact currently
  contains the library, function and independent encoding metadata. `keyFor`
  supplies the logical SHA-256 identity for the compile request/disk artifact.
- A future `MTL4LibraryFunctionDescriptor` references that exact library and
  entry name. Its logical function key is the IR artifact key; its pointer
  fingerprint identifies the live library/function instance. Pointer identity
  is process-local and must never become an archive identity.
- Render calls `MVKMetal4CompilerService::newMTLRenderPipelineState` with the
  existing full legacy descriptor plus both stages' descriptors/logical/pointer
  keys. The service owns base creation, caching and specialization. Compute
  calls `newMTLComputePipelineState` with its library-function descriptor and
  existing threadgroup settings; there is no render-style compute base API.
- Route only when the existing Metal 4 switch and OS/hardware checks pass.
  iOS 17–25 stays on the legacy PSO API. A Metal 4 failure may retry that same
  IR function on the legacy PSO API, retaining reason/count/time. IR rejection
  remains an error; no MSL fallback is introduced.

## Ownership / lifetime observation

The current `BaseEntry` retains input function descriptors while compiling and
releases them after successful completion only when the device has a shared
MSL shader-library repository. An IR device can have no such repository. Before
using that service for IR, verify that completed bases do not keep descriptors
and their libraries pinned outside the independent IR512 cache. The base
pipeline and driver may retain additional references; releasing the descriptor
alone is not proof of lower physical footprint.

Required future checks: base in-flight ownership, artifact eviction while bases
remain live, specialization after input-library release, disk reconstruction of
the same logical key, concurrent create/evict, compiler timeout and device
shutdown. Keep IR encoding metadata independent of library/function wrappers.
Do not alter P0 cache limits, zero semantics or disk-restoration timing.

## Telemetry boundary

Task 3 owns IR-specific base/specialization attempts, successes/failures, total
and max duration, legacy-PSO fallback count/reason/time. Attribute the existing
compiler service work to IR separately from MSL. Disabled/unavailable telemetry
is distinct from an observed zero. The PSO comparison includes base cost,
specialization cost and any fallback work; library load remains separately
reported. P0's library-restoration counters are reused without modification.

Existing flexible-pipeline `fallbacks` includes intentional direct-legacy
bypasses: the diagnostic IR run reported 512 fallbacks with zero Metal 4
attempts. Do not use that counter as the new IR failure count. Count a failure
fallback only after an attempted Metal 4 compile, with its original reason;
missing function descriptors and OS/switch bypasses remain distinct.

Task 4 owns serializer attachment, binary capture, archive identity, flush,
loading, shards/generations and archive telemetry. It can reuse the probe's
immutable metallib manifest and per-row timings. The current probe exposes no
serializer or archive API. Add task 4 experiments as independent files/commits;
do not overwrite the task 3 directory wholesale. Product shared-file changes
must be exchanged as exact commits after the gates permit implementation.

No-Go for flexible-pipeline product integration does not close archive research:
task 4 can compare archived compilation with the legacy PSO path independently.
