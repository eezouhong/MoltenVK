#!/usr/bin/env bash
set -euo pipefail

# Exact-SHA Phase 1C gate: run the same Vulkan binary with the command backend
# disabled and enabled; only MoltenVK's internal backend selection may differ.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD_DIR="${ROOT}/build/metal4-command-backend-e2e"
mkdir -p "${BUILD_DIR}"

print_diagnostics() {
  local status=$?
  if [[ ${status} -ne 0 ]]; then
    echo "Metal 4 Phase 1C E2E failed with status ${status}" >&2
    for log in "${BUILD_DIR}/compile.log" "${BUILD_DIR}/legacy.log" "${BUILD_DIR}/metal4.log"; do
      if [[ -f "${log}" ]]; then
        echo "===== ${log} =====" >&2
        cat "${log}" >&2
      fi
    done
  fi
  exit "${status}"
}
trap print_diagnostics EXIT

DYLIB="$(find "${ROOT}/Package" -path '*/dynamic/dylib/macOS/libMoltenVK.dylib' -print | head -n 1)"
if [[ -z "${DYLIB}" ]]; then
  echo "libMoltenVK.dylib was not produced by the package build" >&2
  exit 1
fi
DYLIB_DIR="$(dirname "${DYLIB}")"

xcrun --sdk macosx clang++ \
  -std=c++17 \
  -Wall -Wextra -Werror \
  -I"${ROOT}/MoltenVK/include" \
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
export MVK_CONFIG_VK_SEMAPHORE_SUPPORT_STYLE=2
export MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0

MVK_CONFIG_METAL4_COMMAND_BACKEND=0 \
  "${BUILD_DIR}/metal4-transfer-e2e" \
  >"${BUILD_DIR}/legacy.log" 2>&1

grep -q 'METAL4_PHASE1C_E2E_PASS' "${BUILD_DIR}/legacy.log"
grep -q 'TIMELINE_OK' "${BUILD_DIR}/legacy.log"
grep -q 'COMPUTE_OK' "${BUILD_DIR}/legacy.log"
grep -q 'IMAGE_DATA_OK' "${BUILD_DIR}/legacy.log"
if grep -q 'Executed first Vulkan submission on the Metal 4 transfer backend' "${BUILD_DIR}/legacy.log"; then
  echo "Metal 4 marker appeared with the backend disabled" >&2
  exit 1
fi

MVK_CONFIG_METAL4_COMMAND_BACKEND=1 \
  "${BUILD_DIR}/metal4-transfer-e2e" \
  >"${BUILD_DIR}/metal4.log" 2>&1

grep -q 'METAL4_PHASE1C_E2E_PASS' "${BUILD_DIR}/metal4.log"
grep -q 'TIMELINE_OK' "${BUILD_DIR}/metal4.log"
grep -q 'COMPUTE_OK' "${BUILD_DIR}/metal4.log"
grep -q 'IMAGE_DATA_OK' "${BUILD_DIR}/metal4.log"
grep -q 'Metal 4 Vulkan transfer backend ready' "${BUILD_DIR}/metal4.log"
grep -q 'Executed first Vulkan submission on the Metal 4 transfer backend' "${BUILD_DIR}/metal4.log"
grep -Eq 'image_copies=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'compute_dispatches=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"
grep -Eq 'barriers=[1-9][0-9]*' "${BUILD_DIR}/metal4.log"

cat "${BUILD_DIR}/legacy.log"
cat "${BUILD_DIR}/metal4.log"
