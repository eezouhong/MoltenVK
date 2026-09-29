# `VK_EXT_mesh_shader` in This MoltenVK Fork

This fork implements `VK_EXT_mesh_shader` on Metal mesh render pipelines. MeloNX uses it to
draw passthrough geometry shaders, such as those in NieR:Automata, as mesh shaders instead
of emulating them with compute dispatches. The compute emulation ends the render pass on
every draw. MeloNX turns this off with its **Geometry Shader Acceleration** setting; see
`docs/architecture/vtg-mesh-shaders.md` in the MeloNX repository.

## Scope

- **Exposed** on devices with Metal 3 and an Apple7 (A14) or later GPU, or a Mac2 GPU, and
  with macOS 13 or iOS 16 or later.
- **Default:** on. Applications opt in by enabling the extension and its `meshShader`
  feature. Nothing changes for applications that don't.
- **Supported:** mesh shaders, `vkCmdDrawMeshTasksEXT` and `vkCmdDrawMeshTasksIndirectEXT`.
- **Not supported:** task shaders, multiview, primitive fragment shading rate, mesh shader
  queries, and `vkCmdDrawMeshTasksIndirectCountEXT`. Using any of these reports
  `VK_ERROR_FEATURE_NOT_PRESENT`.

## Implementation

| Area | Files | Notes |
| --- | --- | --- |
| Extension and features | `Layers/MVKExtensions.def`, `GPUObjects/MVKDeviceFeatureStructs.def`, `GPUObjects/MVKDevice.mm` | `_supportsMeshShaders` gates the extension; limits are in `getProperties()` |
| Pipelines | `GPUObjects/MVKPipeline.mm` (`initMeshMTLRenderPipelineState`) | The MESH stage occupies the vertex stage of the graphics pipeline, including its resources and implicit buffers. Fragment and output state are built as for vertex pipelines and copied into an `MTLMeshRenderPipelineDescriptor`. Vertex input and input assembly state are ignored. Mesh pipelines use the regular Metal pipeline compiler, not the Metal 4 flexible pipeline path |
| Descriptors and push constants | `GPUObjects/MVKDescriptorSet.mm`, `mvkVkShaderStageFlagsBoundToMVKShaderStage` | `VK_SHADER_STAGE_MESH_BIT_EXT` bindings are assigned to the vertex stage slot |
| Resource binding | `Commands/MVKCommandEncoderState.mm` (`MVKMeshBinder`) | Mesh functions bind with the `setMesh*` encoder methods and `MTLRenderStageMesh`. Vertex stage binding state is invalidated when switching between vertex and mesh pipelines |
| Draw commands | `Commands/MVKCmdDraw.mm`, `Vulkan/vulkan.mm` | `drawMeshThreadgroups` and `drawMeshThreadgroupsWithIndirectBuffer`. The indirect command layout matches `MTLDispatchThreadgroupsIndirectArguments`. When the shader reads `DrawIndex`, each draw binds its index with `setMeshBytes` |

## Limits

| Limit | Reported | Vulkan minimum | Reason |
| --- | --- | --- | --- |
| `maxMeshOutputMemorySize` | 30208 | 32768 | Outputs share the 32 KB of threadgroup memory with shared memory, and threadgroup memory plus mesh data is capped at 60 KB |
| `maxMeshOutputComponents` | 120 | 128 | Metal allows 124 mesh output scalars, including the position |
| `maxMeshPayloadAndOutputMemorySize` | 48128 | 48128 | Without task shaders there is no payload |
| `maxMeshSharedMemorySize` | 28672 | 28672 | A shader can't use this and the maximum output size at once |
| `maxMeshOutputVertices` / `Primitives` | 256 / 256 | 256 / 256 | |
| `maxMeshOutputLayers` | `maxFramebufferLayers` | 8 | Per-primitive `render_target_array_index` |

The first two values were measured on Metal. A shader within the reported limits builds;
the smallest shader over them fails. Because these two are below the Vulkan minimums, the
extension is not conformant. Reporting the minimums would move the failures to
applications that check the limits.

## SPIRV-Cross Patches

The SPIRV-Cross revision is pinned in `ExternalRevisions/SPIRV-Cross_repo_revision`. Two
local patches in `ExternalRevisions/patches/SPIRV-Cross` are applied in name order by
`fetchDependencies` and by the CMake build (`cmake/recipes/SPIRV-Cross.cmake`).
`ExternalRevisions/README.md` describes the patch mechanism.

- `0001-MSL-Resolve-LocalSizeId-workgroup-size.patch` fixes shaders that declare their
  workgroup size with `LocalSizeId`. SPIRV-Cross translated them with a zero size: mesh
  shader output loops never advanced and hung the GPU, and `vkCmdDispatchBase` offsets were
  multiplied by zero.
- `0002-MSL-Pass-mesh-shader-builtin-inputs-with-their-builtin-type.patch` fixes mesh
  shaders that read a builtin input whose SPIR-V type differs from its MSL type, such as
  `gl_DrawID`. The mesh entry point declared it as `uint` but passed it to the function body
  as `int&`, so the MSL did not compile.

Both bugs were still present on SPIRV-Cross main on 2026-09-18. When the pinned revision
moves:

- Remove a patch once upstream includes its fix.
- `fetchDependencies` and the CMake configuration stop if a patch no longer applies.

## Testing

**CI** builds the extension on every job; there are no Vulkan CTS runs for it.

**Standalone tests.** The standalone Vulkan tests used for this work live on the MeloNX
webcodex-mac runner, in `/Users/kyle/Documents/MeloNX-debug/moltenvk-mesh-spike-test`. They
link a MoltenVK dylib directly, without the loader.

| Test | Checks |
| --- | --- |
| `mesh_test` | Culling and a subgroup ballot append from mesh shaders |
| `mixed_test` | Vertex and mesh draws in one render pass, with shared descriptors and push constants |
| `mesh_probe shuffle`, `order` and `outmem` | Mesh stage subgroup operations; primitive order across workgroups; output size limits |
| `mesh_review_test drawid` and `layers` | `DrawIndex` for direct and indirect draws; per-primitive layers |
| `run_phase1_matrix.sh` | The tests above under three configurations: default, the MeloNX Metal 4 settings with the legacy command backend, and the same settings with the Metal 4 command backend. It also cross-checks the reported limits |

## Troubleshooting

- **Pipeline creation fails with `Could not compile mesh render pipeline …` or an MSL
  compile error.** Check the shader against the limits above first. Then translate the
  SPIR-V with `MoltenVKShaderConverter -si mesh.spv -mo mesh.metal` and compile the MSL with
  `xcrun -sdk macosx metal -std=metal3.0 -c` to see the Metal error. MeloNX falls back to
  its compute emulation when a mesh pipeline fails.
- **GPU hang in a mesh shader.** Check that patch `0001` is applied. The MSL
  `gl_WorkGroupSize` must not be `uint3(0, 0, 0)`.
- **A `DrawIndex` shader does not compile, or reads a wrong value.** Check that patch
  `0002` is applied and that the draw commands still bind the draw index.
- **Rebuilding after a SPIRV-Cross change.** An incremental `MoltenVKPackaging` build can
  keep the old SPIRV-Cross code, because `libMoltenVKShaderConverter.a` embeds it. Rebuild
  the SPIRV-Cross xcframework first, with `fetchDependencies` or the `SPIRV-Cross-*`
  scheme, then build MoltenVK from a clean derived data directory.
