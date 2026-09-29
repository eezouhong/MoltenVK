# SPIRV-Cross (https://github.com/KhronosGroup/SPIRV-Cross)
# License: Apache-2.0
if(TARGET SPRIV-Cross::SPRIV-Cross)
    return()
endif()

message(STATUS "External: creating target 'SPRIV-Cross::SPRIV-Cross'")

# Read Git commit hash from ExternalRevisions file
file(READ "${MOLTEN_VK_EXTERNAL_REVISIONS_DIR}/SPIRV-Cross_repo_revision" SPIRV_CROSS_COMMIT_HASH)
string(STRIP "${SPIRV_CROSS_COMMIT_HASH}" SPIRV_CROSS_COMMIT_HASH)

# Apply the local patches in ExternalRevisions/patches/SPIRV-Cross in name order, as fetchDependencies does.
# The patched checkout is cached under a key that includes the patch contents, so a changed patch
# gets a fresh checkout. A local source given with CPM_SPIRV-Cross_SOURCE is used as-is.
file(GLOB SPIRV_CROSS_PATCHES "${MOLTEN_VK_EXTERNAL_REVISIONS_DIR}/patches/SPIRV-Cross/*.patch")
list(SORT SPIRV_CROSS_PATCHES)
set(SPIRV_CROSS_PATCH_ARGS "")
if(NOT "${CPM_SPIRV-Cross_SOURCE}" STREQUAL "")
    message(STATUS "Using ${CPM_SPIRV-Cross_SOURCE} as-is; ExternalRevisions/patches/SPIRV-Cross is not applied to it.")
elseif(SPIRV_CROSS_PATCHES)
    set(SPIRV_CROSS_PATCH_HASHES "")
    foreach(SPIRV_CROSS_PATCH IN LISTS SPIRV_CROSS_PATCHES)
        file(SHA1 "${SPIRV_CROSS_PATCH}" SPIRV_CROSS_PATCH_HASH)
        string(APPEND SPIRV_CROSS_PATCH_HASHES "${SPIRV_CROSS_PATCH_HASH};")
    endforeach()
    string(SHA1 SPIRV_CROSS_PATCHES_HASH "${SPIRV_CROSS_PATCH_HASHES}")
    string(SUBSTRING "${SPIRV_CROSS_PATCHES_HASH}" 0 12 SPIRV_CROSS_PATCHES_HASH)
    set(SPIRV_CROSS_PATCH_ARGS
        PATCHES ${SPIRV_CROSS_PATCHES}
        CUSTOM_CACHE_KEY "${SPIRV_CROSS_COMMIT_HASH}-patches-${SPIRV_CROSS_PATCHES_HASH}"
    )
endif()

include(CPM)
CPMAddPackage(
  NAME SPIRV-Cross
  GITHUB_REPOSITORY KhronosGroup/SPIRV-Cross
  GIT_TAG ${SPIRV_CROSS_COMMIT_HASH}
  SYSTEM TRUE
  ${SPIRV_CROSS_PATCH_ARGS}
  OPTIONS
    "SPIRV_CROSS_CLI OFF"
    "SPIRV_CROSS_ENABLE_TESTS OFF"
    "SPIRV_CROSS_ENABLE_GLSL ON"
    "SPIRV_CROSS_ENABLE_HLSL OFF"
    "SPIRV_CROSS_ENABLE_MSL ON"
    "SPIRV_CROSS_ENABLE_CPP OFF"
    "SPIRV_CROSS_ENABLE_REFLECT ON"
    "SPIRV_CROSS_ENABLE_C_API OFF"
    "SPIRV_CROSS_ENABLE_UTIL OFF"
    "SPIRV_CROSS_NAMESPACE_OVERRIDE MVK_spirv_cross"
    "SPIRV_CROSS_SKIP_INSTALL ON"
)

add_library(SPRIV-Cross::Core ALIAS spirv-cross-core)
add_library(SPRIV-Cross::Reflect ALIAS spirv-cross-reflect)
add_library(SPRIV-Cross::GLSL ALIAS spirv-cross-glsl)
add_library(SPRIV-Cross::MSL ALIAS spirv-cross-msl)

add_library(SPRIV-Cross INTERFACE)
add_library(SPRIV-Cross::SPRIV-Cross ALIAS SPRIV-Cross)
target_link_libraries(SPRIV-Cross INTERFACE
    spirv-cross-core
    spirv-cross-reflect
    spirv-cross-glsl
    spirv-cross-msl
)
