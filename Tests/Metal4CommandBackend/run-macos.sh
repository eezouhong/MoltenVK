#!/usr/bin/env bash
set -euo pipefail

# Exact-SHA Phase 1C gate after fully zero-initializing every Vulkan structure
# used by the independent test: run the same binary with the command backend
# disabled and enabled; only MoltenVK's internal backend selection may differ.
# CI invokes this script through bash, so validation does not depend on Git's executable bit.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ROOT}/build/metal4-command-backend-e2e"
mkdir -p "${BUILD_DIR}"

print_diagnostics() {
  local status=$?
  if [[ ${status} -ne 0 ]]; then
    echo "Metal 4 Phase 1C E2E failed with status ${status}" >&2
    for log in "${BUILD_DIR}/compile.log" "${BUILD_DIR}/legacy.log" "${BUILD_DIR}/metal4.log" "${BUILD_DIR}/metal4-single-queue.log"; do
      if [[ -f "${log}" ]]; then
        echo "===== ${log} =====" >&2
        cat "${log}" >&2
      fi
    done
  fi
  exit "${status}"
}
trap print_diagnostics EXIT

# Current packages place the dylib inside MoltenVK.xcframework. Keep the lookup
# independent of the XCFramework slice directory name and architecture spelling.
DYLIB="$(find "${ROOT}/Package" -type f -name 'libMoltenVK.dylib' -print | head -n 1)"
if [[ -z "${DYLIB}" ]]; then
  echo "libMoltenVK.dylib was not produced by the package build" >&2
  find "${ROOT}/Package" -maxdepth 6 -type f -print >&2 || true
  exit 1
fi
DYLIB_DIR="$(dirname "${DYLIB}")"

# A normal source build provides Vulkan-Headers under External. The fast E2E
# path intentionally reuses a verified package, which carries the same public
# Vulkan headers under Package/MoltenVK/MoltenVK/include.
INCLUDE_ARGS=("-I${ROOT}/MoltenVK/include")
if [[ -f "${ROOT}/External/Vulkan-Headers/include/vulkan/vulkan.h" ]]; then
  INCLUDE_ARGS+=("-I${ROOT}/External/Vulkan-Headers/include")
fi
if [[ -f "${ROOT}/Package/MoltenVK/MoltenVK/include/vulkan/vulkan.h" ]]; then
  INCLUDE_ARGS+=("-I${ROOT}/Package/MoltenVK/MoltenVK/include")
fi

xcrun --sdk macosx clang++ \
  -std=c++17 \
  -Wall -Wextra -Werror \
  "${INCLUDE_ARGS[@]}" \
  "${ROOT}/Tests/Metal4CommandBackend/metal4_transfer_e2e.cpp" \
  "${DYLIB}" \
  -Wl,-rpath,"${DYLIB_DIR}" \
  -framework Metal \
  -framework Foundation \
  -framework QuartzCore \
  -framework IOSurface \
  -framework IOKit \
  -o "${BUILD_DIR}/metal4-transfer-e2e" \
  >"${BUILD_DIR}/compile.log" 2>&1

export MVK_CONFIG_LOG_LEVEL=3
export MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0

MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=2 \
MVK_CONFIG_METAL4_COMMAND_BACKEND=0 \
  "${BUILD_DIR}/metal4-transfer-e2e" \
  >"${BUILD_DIR}/legacy.log" 2>&1

grep -q 'METAL4_PHASE1C_E2E_PASS' "${BUILD_DIR}/legacy.log"
grep -q 'TIMELINE_OK' "${BUILD_DIR}/legacy.log"
grep -q 'ALLOCATOR_REUSE_BURST_OK' "${BUILD_DIR}/legacy.log"
grep -q 'COMPUTE_OK' "${BUILD_DIR}/legacy.log"
grep -q 'DESCRIPTOR_COMPUTE_OK' "${BUILD_DIR}/legacy.log"
grep -q 'IMAGE_DATA_OK' "${BUILD_DIR}/legacy.log"
grep -q 'GENERAL_LAYOUT_IMAGE_OK' "${BUILD_DIR}/legacy.log"
grep -q 'PRESENT_LAYOUT_AND_QUEUE_OK' "${BUILD_DIR}/legacy.log"
grep -q 'BUFFER_IMAGE_LAYERED_OK' "${BUILD_DIR}/legacy.log"
grep -q 'BUFFER_IMAGE_3D_OK' "${BUILD_DIR}/legacy.log"
grep -q 'BUFFER_IMAGE_DEPTH_STENCIL_OK' "${BUILD_DIR}/legacy.log"
grep -q 'RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'RASTERIZER_DISCARD_OK' "${BUILD_DIR}/legacy.log"
grep -q 'INDEXED_DRAW_OK' "${BUILD_DIR}/legacy.log"
grep -q 'DESCRIPTOR_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'UNUSED_DESCRIPTOR_LIFETIME_OK' "${BUILD_DIR}/legacy.log"
grep -q 'VERTEX_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'DYNAMIC_VERTEX_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'DYNAMIC_VIEWPORT_SCISSOR_OK' "${BUILD_DIR}/legacy.log"
grep -q 'OVERSIZED_SCISSOR_CLIP_OK' "${BUILD_DIR}/legacy.log"
grep -q 'MULTI_VIEWPORT_SCISSOR_OK' "${BUILD_DIR}/legacy.log"
grep -q 'ACTIVE_BLEND_CONSTANTS_OK' "${BUILD_DIR}/legacy.log"
grep -q 'INACTIVE_STENCIL_DYNAMIC_OK' "${BUILD_DIR}/legacy.log"
grep -q 'INACTIVE_DEPTH_BIAS_DYNAMIC_OK' "${BUILD_DIR}/legacy.log"
grep -q 'ACTIVE_DYNAMIC_DEPTH_BIAS_OK' "${BUILD_DIR}/legacy.log"
grep -q 'DEPTH_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'CLASSIC_DEPTH_ONLY_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'CLASSIC_MRT_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'CLASSIC_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/legacy.log"
grep -q 'DYNAMIC_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/legacy.log"
grep -q 'CLASSIC_LAYERED_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'CLASSIC_INACTIVE_STENCIL_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'CLASSIC_STATIC_STENCIL_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'CLASSIC_CLEAR_STENCIL_OK' "${BUILD_DIR}/legacy.log"
grep -q 'DYNAMIC_STATIC_STENCIL_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'DYNAMIC_STENCIL_VALUES_OK' "${BUILD_DIR}/legacy.log"
grep -q 'QUERY_RESET_OK' "${BUILD_DIR}/legacy.log"
grep -q 'QUERY_OCCLUSION_OK' "${BUILD_DIR}/legacy.log"
grep -q 'QUERY_COPY_RESULTS_OK' "${BUILD_DIR}/legacy.log"
grep -q 'QUERY_COPY_ACROSS_EMPTY_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'MULTI_QUERY_POOL_RENDER_SCOPES_OK' "${BUILD_DIR}/legacy.log"
grep -q 'MULTI_QUERY_POOL_SINGLE_RENDER_SCOPE_OK' "${BUILD_DIR}/legacy.log"
grep -q 'QUERY_REUSE_AFTER_RESET_OK' "${BUILD_DIR}/legacy.log"
grep -q 'COMPUTE_REBIND_AFTER_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'QUERY_OUTSIDE_RENDER_SCOPE_OK' "${BUILD_DIR}/legacy.log"
grep -q 'QUERY_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/legacy.log"
grep -q 'QUERY_PARTIAL_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/legacy.log"
grep -q 'QUERY_CLEAR_EXCLUDED_FROM_OCCLUSION_OK' "${BUILD_DIR}/legacy.log"
grep -q 'RENDER_SCOPE_PIPELINE_BARRIER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'RENDER_SCOPE_BATCHED_PIPELINE_BARRIER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'GRAPHICS_REBIND_AFTER_RENDER_OK' "${BUILD_DIR}/legacy.log"
grep -q 'UPDATE_BUFFER_OK' "${BUILD_DIR}/legacy.log"
if grep -q 'Executed first Vulkan submission on the Metal 4 transfer backend' "${BUILD_DIR}/legacy.log"; then
  echo "Metal 4 marker appeared with the backend disabled" >&2
  exit 1
fi

MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=2 \
MVK_CONFIG_METAL4_COMMAND_BACKEND=1 \
MVK_CONFIG_METAL4_COMMAND_TELEMETRY=1 \
MVK_CONFIG_METAL4_COMMAND_VALIDATION=1 \
  "${BUILD_DIR}/metal4-transfer-e2e" \
  >"${BUILD_DIR}/metal4.log" 2>&1

if ! grep -q 'Metal 4 Vulkan transfer backend ready' "${BUILD_DIR}/metal4.log"; then
  if grep -Eq 'Could not create the Metal 4 command queue.*Device does not support Metal 4' "${BUILD_DIR}/metal4.log"; then
    grep -q 'METAL4_PHASE1C_E2E_PASS' "${BUILD_DIR}/metal4.log"
    echo 'METAL4_BACKEND_E2E_SKIPPED_UNSUPPORTED_DEVICE'
    cat "${BUILD_DIR}/legacy.log"
    cat "${BUILD_DIR}/metal4.log"
    exit 0
  fi

  echo 'Metal 4 command backend did not become ready for an unexpected reason' >&2
  exit 1
fi

grep -q 'METAL4_PHASE1C_E2E_PASS' "${BUILD_DIR}/metal4.log"
grep -q 'TIMELINE_OK' "${BUILD_DIR}/metal4.log"
grep -q 'ALLOCATOR_REUSE_BURST_OK' "${BUILD_DIR}/metal4.log"
grep -q 'COMPUTE_OK' "${BUILD_DIR}/metal4.log"
grep -q 'DESCRIPTOR_COMPUTE_OK' "${BUILD_DIR}/metal4.log"
grep -q 'IMAGE_DATA_OK' "${BUILD_DIR}/metal4.log"
grep -q 'GENERAL_LAYOUT_IMAGE_OK' "${BUILD_DIR}/metal4.log"
grep -q 'PRESENT_LAYOUT_AND_QUEUE_OK' "${BUILD_DIR}/metal4.log"
grep -q 'BUFFER_IMAGE_LAYERED_OK' "${BUILD_DIR}/metal4.log"
grep -q 'BUFFER_IMAGE_3D_OK' "${BUILD_DIR}/metal4.log"
grep -q 'BUFFER_IMAGE_DEPTH_STENCIL_OK' "${BUILD_DIR}/metal4.log"
grep -q 'RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'RASTERIZER_DISCARD_OK' "${BUILD_DIR}/metal4.log"
grep -q 'INDEXED_DRAW_OK' "${BUILD_DIR}/metal4.log"
grep -q 'DESCRIPTOR_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'UNUSED_DESCRIPTOR_LIFETIME_OK' "${BUILD_DIR}/metal4.log"
grep -q 'VERTEX_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'DYNAMIC_VERTEX_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'DYNAMIC_VIEWPORT_SCISSOR_OK' "${BUILD_DIR}/metal4.log"
grep -q 'OVERSIZED_SCISSOR_CLIP_OK' "${BUILD_DIR}/metal4.log"
grep -q 'MULTI_VIEWPORT_SCISSOR_OK' "${BUILD_DIR}/metal4.log"
grep -q 'ACTIVE_BLEND_CONSTANTS_OK' "${BUILD_DIR}/metal4.log"
grep -q 'INACTIVE_STENCIL_DYNAMIC_OK' "${BUILD_DIR}/metal4.log"
grep -q 'INACTIVE_DEPTH_BIAS_DYNAMIC_OK' "${BUILD_DIR}/metal4.log"
grep -q 'ACTIVE_DYNAMIC_DEPTH_BIAS_OK' "${BUILD_DIR}/metal4.log"
grep -q 'DEPTH_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'CLASSIC_DEPTH_ONLY_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'CLASSIC_MRT_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'CLASSIC_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/metal4.log"
grep -q 'DYNAMIC_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/metal4.log"
grep -q 'CLASSIC_LAYERED_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'CLASSIC_INACTIVE_STENCIL_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'CLASSIC_STATIC_STENCIL_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'CLASSIC_CLEAR_STENCIL_OK' "${BUILD_DIR}/metal4.log"
grep -q 'DYNAMIC_STATIC_STENCIL_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'DYNAMIC_STENCIL_VALUES_OK' "${BUILD_DIR}/metal4.log"
grep -q 'QUERY_RESET_OK' "${BUILD_DIR}/metal4.log"
grep -q 'QUERY_OCCLUSION_OK' "${BUILD_DIR}/metal4.log"
grep -q 'QUERY_COPY_RESULTS_OK' "${BUILD_DIR}/metal4.log"
grep -q 'QUERY_COPY_ACROSS_EMPTY_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'MULTI_QUERY_POOL_RENDER_SCOPES_OK' "${BUILD_DIR}/metal4.log"
grep -q 'MULTI_QUERY_POOL_SINGLE_RENDER_SCOPE_OK' "${BUILD_DIR}/metal4.log"
grep -q 'QUERY_REUSE_AFTER_RESET_OK' "${BUILD_DIR}/metal4.log"
grep -q 'COMPUTE_REBIND_AFTER_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'QUERY_OUTSIDE_RENDER_SCOPE_OK' "${BUILD_DIR}/metal4.log"
grep -q 'QUERY_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/metal4.log"
grep -q 'QUERY_PARTIAL_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/metal4.log"
grep -q 'QUERY_CLEAR_EXCLUDED_FROM_OCCLUSION_OK' "${BUILD_DIR}/metal4.log"
grep -q 'RENDER_SCOPE_PIPELINE_BARRIER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'RENDER_SCOPE_BATCHED_PIPELINE_BARRIER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'GRAPHICS_REBIND_AFTER_RENDER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'UPDATE_BUFFER_OK' "${BUILD_DIR}/metal4.log"
grep -q 'Metal 4 Vulkan transfer backend ready' "${BUILD_DIR}/metal4.log"
grep -q 'Executed first Vulkan submission on the Metal 4 transfer backend' "${BUILD_DIR}/metal4.log"
grep -q 'Metal 4 command backend telemetry:' "${BUILD_DIR}/metal4.log"
grep -Eq 'coverage_ppm=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'Metal 4 command backend timing: .*attempt_total_ns=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'image_copies=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'compute_dispatches=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'compute_dispatches=([2-9]|[1-9][0-9]+)' "${BUILD_DIR}/metal4.log"
grep -Eq 'render_submissions=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'render_passes=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'draws=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'barriers=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'query_resets=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'query_copies=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'buffer_updates=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'Metal 4 command backend summary: .*fallbacks=0, failures=0' "${BUILD_DIR}/metal4.log"
if grep -q 'Metal 4 command materialization failed' "${BUILD_DIR}/metal4.log"; then
  echo "Controlled Metal 4 run unexpectedly replayed a command through the legacy backend" >&2
  exit 1
fi
grep -q 'unsupported_commands=none' "${BUILD_DIR}/metal4.log"

MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=0 \
MVK_CONFIG_METAL4_COMMAND_BACKEND=1 \
MVK_CONFIG_METAL4_COMMAND_TELEMETRY=1 \
MVK_CONFIG_METAL4_COMMAND_VALIDATION=1 \
  "${BUILD_DIR}/metal4-transfer-e2e" \
  >"${BUILD_DIR}/metal4-single-queue.log" 2>&1

grep -q 'METAL4_PHASE1C_E2E_PASS' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'TIMELINE_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'ALLOCATOR_REUSE_BURST_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'DESCRIPTOR_COMPUTE_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'GENERAL_LAYOUT_IMAGE_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'PRESENT_LAYOUT_AND_QUEUE_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'BUFFER_IMAGE_LAYERED_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'BUFFER_IMAGE_3D_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'BUFFER_IMAGE_DEPTH_STENCIL_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'RASTERIZER_DISCARD_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'INDEXED_DRAW_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'UNUSED_DESCRIPTOR_LIFETIME_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'VERTEX_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'DYNAMIC_VERTEX_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'DYNAMIC_VIEWPORT_SCISSOR_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'OVERSIZED_SCISSOR_CLIP_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'MULTI_VIEWPORT_SCISSOR_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'ACTIVE_BLEND_CONSTANTS_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'INACTIVE_STENCIL_DYNAMIC_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'INACTIVE_DEPTH_BIAS_DYNAMIC_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'ACTIVE_DYNAMIC_DEPTH_BIAS_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'DEPTH_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'CLASSIC_DEPTH_ONLY_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'CLASSIC_MRT_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'CLASSIC_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'DYNAMIC_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'CLASSIC_LAYERED_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'CLASSIC_INACTIVE_STENCIL_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'CLASSIC_STATIC_STENCIL_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'CLASSIC_CLEAR_STENCIL_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'DYNAMIC_STATIC_STENCIL_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'DYNAMIC_STENCIL_VALUES_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'QUERY_COPY_RESULTS_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'QUERY_COPY_ACROSS_EMPTY_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'MULTI_QUERY_POOL_RENDER_SCOPES_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'MULTI_QUERY_POOL_SINGLE_RENDER_SCOPE_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'QUERY_REUSE_AFTER_RESET_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'COMPUTE_REBIND_AFTER_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'QUERY_OUTSIDE_RENDER_SCOPE_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'QUERY_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'QUERY_PARTIAL_CLEAR_ATTACHMENTS_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'QUERY_CLEAR_EXCLUDED_FROM_OCCLUSION_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'RENDER_SCOPE_PIPELINE_BARRIER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'RENDER_SCOPE_BATCHED_PIPELINE_BARRIER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'GRAPHICS_REBIND_AFTER_RENDER_OK' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'Executed first Vulkan submission on the Metal 4 transfer backend' "${BUILD_DIR}/metal4-single-queue.log"
grep -q 'Metal 4 command backend telemetry:' "${BUILD_DIR}/metal4-single-queue.log"
grep -Eq 'coverage_ppm=[1-9][0-9]*' "${BUILD_DIR}/metal4-single-queue.log"
grep -Eq 'Metal 4 command backend timing: .*attempt_total_ns=[1-9][0-9]*' "${BUILD_DIR}/metal4-single-queue.log"
grep -Eq 'Metal 4 command backend summary: .*fallbacks=0, failures=0' "${BUILD_DIR}/metal4-single-queue.log"
if grep -q 'Metal 4 command materialization failed' "${BUILD_DIR}/metal4-single-queue.log"; then
  echo "Single-queue Metal 4 run unexpectedly replayed a command through the legacy backend" >&2
  exit 1
fi
grep -q 'unsupported_commands=none' "${BUILD_DIR}/metal4-single-queue.log"
if grep -q 'latest_fallback=unsupported_semaphore' "${BUILD_DIR}/metal4-single-queue.log"; then
  echo "Single-queue semaphore unexpectedly forced Metal 4 fallback" >&2
  exit 1
fi

cat "${BUILD_DIR}/legacy.log"
cat "${BUILD_DIR}/metal4.log"
cat "${BUILD_DIR}/metal4-single-queue.log"
