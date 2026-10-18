cmake_minimum_required(VERSION 3.20)
project(MetalIRSystemValueReplay LANGUAGES CXX)

set(VULKAN_HEADERS "" CACHE PATH "Directory containing vulkan/vulkan.h")
set(MOLTENVK_LIBRARY "" CACHE FILEPATH "Built macOS MoltenVK dylib")
if(NOT EXISTS "${VULKAN_HEADERS}/vulkan/vulkan.h" OR NOT EXISTS "${MOLTENVK_LIBRARY}")
    message(FATAL_ERROR "Set VULKAN_HEADERS and MOLTENVK_LIBRARY explicitly")
endif()
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
add_executable(msc_negative_probe msc_negative_probe.cpp)
target_compile_features(msc_negative_probe PRIVATE cxx_std_17)
target_include_directories(msc_negative_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(msc_negative_probe PRIVATE "${MOLTENVK_LIBRARY}")
add_executable(runtime_probe runtime_probe.cpp)
target_compile_features(runtime_probe PRIVATE cxx_std_17)
target_include_directories(runtime_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(runtime_probe PRIVATE "${MOLTENVK_LIBRARY}")
get_filename_component(MOLTENVK_DIRECTORY "${MOLTENVK_LIBRARY}" DIRECTORY)
set_target_properties(runtime_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")

set(SHADER_OUTPUTS)
foreach(SHADER native_ids.vert msc_negative.comp runtime.vert runtime.frag runtime.comp unused.comp descriptors.comp points.vert points.frag points_plain.frag)
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}.spv")
    add_custom_command(OUTPUT "${OUTPUT}"
        COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
            "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}" -o "${OUTPUT}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}" VERBATIM)
    list(APPEND SHADER_OUTPUTS "${OUTPUT}")
endforeach()
add_custom_target(replay_shaders ALL DEPENDS ${SHADER_OUTPUTS})
add_dependencies(runtime_probe replay_shaders)
set_target_properties(msc_negative_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(msc_negative_probe replay_shaders)
add_executable(native_ids_probe native_ids_probe.cpp)
target_compile_features(native_ids_probe PRIVATE cxx_std_17)
target_include_directories(native_ids_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(native_ids_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(native_ids_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(native_ids_probe replay_shaders)

add_executable(concurrent_probe concurrent_probe.cpp)
target_compile_features(concurrent_probe PRIVATE cxx_std_17)
target_include_directories(concurrent_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(concurrent_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(concurrent_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(concurrent_probe replay_shaders)
add_executable(point_probe point_probe.cpp)
target_compile_features(point_probe PRIVATE cxx_std_17)
target_include_directories(point_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(point_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(point_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(point_probe replay_shaders)
add_executable(descriptor_probe descriptor_probe.cpp)
target_compile_features(descriptor_probe PRIVATE cxx_std_17)
target_include_directories(descriptor_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(descriptor_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(descriptor_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(descriptor_probe replay_shaders)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(SPIRV_DIS spirv-dis REQUIRED)
find_program(SPIRV_AS spirv-as REQUIRED)
find_program(SPIRV_VAL spirv-val REQUIRED)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/signed_modulo.spv"
    COMMAND "${SPIRV_AS}" --target-env spv1.3 "${CMAKE_CURRENT_SOURCE_DIR}/signed_modulo.spvasm"
        -o "${CMAKE_CURRENT_BINARY_DIR}/signed_modulo.spv"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${CMAKE_CURRENT_BINARY_DIR}/signed_modulo.spv"
    DEPENDS signed_modulo.spvasm VERBATIM)
add_custom_target(signed_modulo_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/signed_modulo.spv")
add_executable(signed_modulo_probe signed_modulo_probe.cpp)
target_compile_features(signed_modulo_probe PRIVATE cxx_std_17)
target_include_directories(signed_modulo_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(signed_modulo_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(signed_modulo_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(signed_modulo_probe signed_modulo_shader)

foreach(NAME strong_fma nocontraction_vector nan_default)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv"
        COMMAND "${SPIRV_AS}" --target-env spv1.3 "${CMAKE_CURRENT_SOURCE_DIR}/${NAME}.spvasm"
            -o "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv"
        COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv"
        DEPENDS "${NAME}.spvasm" VERBATIM)
    add_custom_target(${NAME}_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv")
    add_executable(${NAME}_probe "${NAME}_probe.cpp")
    target_compile_features(${NAME}_probe PRIVATE cxx_std_17)
    target_compile_options(${NAME}_probe PRIVATE -ffp-contract=off)
    target_include_directories(${NAME}_probe PRIVATE "${VULKAN_HEADERS}")
    target_link_libraries(${NAME}_probe PRIVATE "${MOLTENVK_LIBRARY}")
    set_target_properties(${NAME}_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
    add_dependencies(${NAME}_probe ${NAME}_shader)
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/strong_fma16.spv"
    COMMAND "${SPIRV_AS}" --target-env spv1.3 "${CMAKE_CURRENT_SOURCE_DIR}/strong_fma16.spvasm"
        -o "${CMAKE_CURRENT_BINARY_DIR}/strong_fma16.spv"
    COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${CMAKE_CURRENT_BINARY_DIR}/strong_fma16.spv"
    DEPENDS strong_fma16.spvasm VERBATIM)
add_custom_target(strong_fma16_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/strong_fma16.spv")
add_dependencies(strong_fma_probe strong_fma16_shader)
set(STRONG_FMA_RULES_SHADERS)
foreach(NAME strong_fma_rules strong_fma_rules16)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv"
        COMMAND "${SPIRV_AS}" --target-env spv1.3 "${CMAKE_CURRENT_SOURCE_DIR}/${NAME}.spvasm"
            -o "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv"
        COMMAND "${SPIRV_VAL}" --target-env vulkan1.2 "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv"
        DEPENDS "${NAME}.spvasm" VERBATIM)
    list(APPEND STRONG_FMA_RULES_SHADERS "${CMAKE_CURRENT_BINARY_DIR}/${NAME}.spv")
endforeach()
add_custom_target(strong_fma_rules_shader DEPENDS ${STRONG_FMA_RULES_SHADERS})
add_executable(strong_fma_rules_probe strong_fma_rules_probe.cpp)
target_compile_features(strong_fma_rules_probe PRIVATE cxx_std_17)
target_compile_options(strong_fma_rules_probe PRIVATE -ffp-contract=off)
target_include_directories(strong_fma_rules_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(strong_fma_rules_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(strong_fma_rules_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(strong_fma_rules_probe strong_fma_rules_shader)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/minmax-safe.spv"
                         "${CMAKE_CURRENT_BINARY_DIR}/minmax-relaxed.spv"
                         "${CMAKE_CURRENT_BINARY_DIR}/minmax-fast.spv"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_minmax.py" --output "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS prepare_minmax.py VERBATIM)
add_custom_target(minmax_shader DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/minmax-safe.spv"
    "${CMAKE_CURRENT_BINARY_DIR}/minmax-relaxed.spv" "${CMAKE_CURRENT_BINARY_DIR}/minmax-fast.spv")
add_executable(minmax_probe minmax_probe.cpp)
target_compile_features(minmax_probe PRIVATE cxx_std_17)
target_compile_options(minmax_probe PRIVATE -ffp-contract=off)
target_include_directories(minmax_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(minmax_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(minmax_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(minmax_probe minmax_shader)
set(MATH_LEGACY "${CMAKE_CURRENT_BINARY_DIR}/math-legacy.spv")
set(MATH_VARIANTS)
foreach(KIND safe mixed-precise relaxed fast relaxed-transform mixed-no-contraction fast-transform)
    list(APPEND MATH_VARIANTS "${CMAKE_CURRENT_BINARY_DIR}/math-${KIND}.spv")
endforeach()
add_custom_command(OUTPUT "${MATH_LEGACY}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
        "${CMAKE_CURRENT_SOURCE_DIR}/relaxed_math.comp" -o "${MATH_LEGACY}"
    DEPENDS relaxed_math.comp VERBATIM)
add_custom_command(OUTPUT ${MATH_VARIANTS}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_math.py"
        --input "${MATH_LEGACY}" --output "${CMAKE_CURRENT_BINARY_DIR}"
        --disassembler "${SPIRV_DIS}" --assembler "${SPIRV_AS}" --validator "${SPIRV_VAL}"
    DEPENDS "${MATH_LEGACY}" prepare_math.py VERBATIM)
add_custom_target(math_shaders ALL DEPENDS ${MATH_VARIANTS})
add_executable(math_probe math_probe.cpp)
target_compile_features(math_probe PRIVATE cxx_std_17)
target_compile_options(math_probe PRIVATE -ffp-contract=off)
target_include_directories(math_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(math_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(math_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(math_probe math_shaders)

set(FMA_LEGACY "${CMAKE_CURRENT_BINARY_DIR}/fma-legacy.spv")
set(FMA_VARIANTS "${CMAKE_CURRENT_BINARY_DIR}/fma-relaxed.spv" "${CMAKE_CURRENT_BINARY_DIR}/fma-fast.spv")
add_custom_command(OUTPUT "${FMA_LEGACY}"
    COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
        "${CMAKE_CURRENT_SOURCE_DIR}/fused_fma.comp" -o "${FMA_LEGACY}"
    DEPENDS fused_fma.comp VERBATIM)
add_custom_command(OUTPUT ${FMA_VARIANTS}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_fma.py"
        --input "${FMA_LEGACY}" --output "${CMAKE_CURRENT_BINARY_DIR}"
        --disassembler "${SPIRV_DIS}" --assembler "${SPIRV_AS}" --validator "${SPIRV_VAL}"
    DEPENDS "${FMA_LEGACY}" prepare_fma.py VERBATIM)
add_custom_target(fma_shaders ALL DEPENDS "${FMA_LEGACY}" ${FMA_VARIANTS})
add_executable(fma_probe fma_probe.cpp)
target_compile_features(fma_probe PRIVATE cxx_std_17)
target_compile_options(fma_probe PRIVATE -ffp-contract=off)
target_include_directories(fma_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(fma_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(fma_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(fma_probe fma_shaders)

set(GRAPHICS_MATH_OUTPUTS)
foreach(STAGE vert frag)
    list(APPEND GRAPHICS_MATH_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/math_plain.${STAGE}.spv")
    foreach(VARIANT no-transform transform)
        list(APPEND GRAPHICS_MATH_OUTPUTS "${CMAKE_CURRENT_BINARY_DIR}/math_graphics-${VARIANT}.${STAGE}.spv")
    endforeach()
endforeach()
add_custom_command(OUTPUT ${GRAPHICS_MATH_OUTPUTS}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_graphics_math.py"
        --source "${CMAKE_CURRENT_SOURCE_DIR}" --output "${CMAKE_CURRENT_BINARY_DIR}"
        --glslang "${GLSLANG_VALIDATOR}" --disassembler "${SPIRV_DIS}"
        --assembler "${SPIRV_AS}" --validator "${SPIRV_VAL}"
    DEPENDS prepare_graphics_math.py prepare_math.py math_graphics.vert math_graphics.frag
            math_plain.vert math_plain.frag VERBATIM)
add_custom_target(graphics_math_shaders ALL DEPENDS ${GRAPHICS_MATH_OUTPUTS})
add_executable(graphics_math_probe graphics_math_probe.cpp)
target_compile_features(graphics_math_probe PRIVATE cxx_std_17)
target_compile_options(graphics_math_probe PRIVATE -ffp-contract=off)
target_include_directories(graphics_math_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(graphics_math_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(graphics_math_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(graphics_math_probe graphics_math_shaders)

add_executable(route_probe route_probe.cpp)
target_compile_features(route_probe PRIVATE cxx_std_17)
target_include_directories(route_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(route_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(route_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")

add_executable(rejection_probe rejection_probe.cpp)
target_compile_features(rejection_probe PRIVATE cxx_std_17)
target_include_directories(rejection_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(rejection_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(rejection_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(rejection_probe replay_shaders)
add_executable(layer_probe layer_probe.cpp)
target_compile_features(layer_probe PRIVATE cxx_std_17)
target_include_directories(layer_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(layer_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(layer_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
set(LAYER_SHADERS)
foreach(SHADER layer.vert viewport.vert layer.frag)
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}.spv")
    add_custom_command(OUTPUT "${OUTPUT}" COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.2 "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}" -o "${OUTPUT}" DEPENDS "${SHADER}" VERBATIM)
    list(APPEND LAYER_SHADERS "${OUTPUT}")
endforeach()
add_custom_target(layer_shaders ALL DEPENDS ${LAYER_SHADERS})
add_dependencies(layer_probe layer_shaders)
add_executable(discard_probe discard_probe.cpp)
target_compile_features(discard_probe PRIVATE cxx_std_17)
target_include_directories(discard_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(discard_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(discard_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
set(DISCARD_SHADERS)
foreach(SHADER discard_producer.vert discard_positionless.vert discard_consumer.vert discard_guard.frag discard_consumer.frag)
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}.spv")
    add_custom_command(OUTPUT "${OUTPUT}" COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1 "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}" -o "${OUTPUT}" DEPENDS "${SHADER}" VERBATIM)
    list(APPEND DISCARD_SHADERS "${OUTPUT}")
endforeach()
add_custom_target(discard_shaders ALL DEPENDS ${DISCARD_SHADERS})
add_dependencies(discard_probe discard_shaders)

# This probe requires the actual Vulkan Loader instead of direct MoltenVK entrypoints.
set(VULKAN_LOADER_LIBRARY "" CACHE FILEPATH "Optional macOS Vulkan Loader for dispatch-lifetime regression")
if(APPLE AND EXISTS "${VULKAN_LOADER_LIBRARY}")
    enable_language(OBJCXX)
    add_executable(loader_handle_probe loader_handle_probe.mm)
    target_compile_features(loader_handle_probe PRIVATE cxx_std_17)
    target_compile_options(loader_handle_probe PRIVATE -fno-rtti)
    target_include_directories(loader_handle_probe PRIVATE "${VULKAN_HEADERS}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../Common"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../MoltenVK/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../MoltenVK/MoltenVK/GPUObjects"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../MoltenVK/MoltenVK/Utility"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../MoltenVK/MoltenVK/OS"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../MoltenVK/MoltenVK/API"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../MoltenVK/MoltenVK/Vulkan")
    target_link_libraries(loader_handle_probe PRIVATE "${VULKAN_LOADER_LIBRARY}" "-framework Foundation" "-framework Metal" "-framework QuartzCore")
endif()

add_executable(varying_probe varying_probe.cpp)
target_compile_features(varying_probe PRIVATE cxx_std_17)
target_include_directories(varying_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(varying_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(varying_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
set(VARYING_SHADERS)
foreach(KIND sparse reordered high array components flat gradient)
    foreach(STAGE vert frag)
        list(APPEND VARYING_SHADERS "${CMAKE_CURRENT_BINARY_DIR}/varying-${KIND}.${STAGE}.spv")
    endforeach()
endforeach()
add_custom_command(OUTPUT ${VARYING_SHADERS}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_varyings.py"
        --output "${CMAKE_CURRENT_BINARY_DIR}" --glslang "${GLSLANG_VALIDATOR}" --validator "${SPIRV_VAL}"
    DEPENDS prepare_varyings.py VERBATIM)
add_custom_target(varying_shaders ALL DEPENDS ${VARYING_SHADERS})
add_dependencies(varying_probe varying_shaders)

# Disk cache tiers only; needs no device.
add_executable(disk_cache_probe disk_cache_probe.cpp)
target_compile_features(disk_cache_probe PRIVATE cxx_std_17)
target_include_directories(disk_cache_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../MoltenVK/MoltenVK/GPUObjects")
