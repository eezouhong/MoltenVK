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
