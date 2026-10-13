# Optional additive fixture; existing replay targets and helpers stay intact.
foreach(SHADER correctness_formats.vert correctness_formats.frag correctness_formats_uint.frag)
    set(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${SHADER}.spv")
    add_custom_command(OUTPUT "${OUTPUT}" COMMAND "${GLSLANG_VALIDATOR}" -V --target-env vulkan1.1
        "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}" -o "${OUTPUT}"
        COMMAND "${SPIRV_VAL}" --target-env vulkan1.1 "${OUTPUT}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER}" VERBATIM)
    list(APPEND CORRECTNESS_FORMAT_SHADERS "${OUTPUT}")
endforeach()
add_custom_target(correctness_format_shaders DEPENDS ${CORRECTNESS_FORMAT_SHADERS})
add_executable(correctness_formats_probe correctness_formats_probe.cpp)
target_compile_features(correctness_formats_probe PRIVATE cxx_std_17)
target_include_directories(correctness_formats_probe PRIVATE "${VULKAN_HEADERS}")
target_link_libraries(correctness_formats_probe PRIVATE "${MOLTENVK_LIBRARY}")
set_target_properties(correctness_formats_probe PROPERTIES BUILD_RPATH "${MOLTENVK_DIRECTORY}")
add_dependencies(correctness_formats_probe correctness_format_shaders)
