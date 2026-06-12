cmake_minimum_required(VERSION 3.22.1)
project(calculator)

set(NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)

set(SLANGC          "C:/VulkanSDK/1.4.341.1/Bin/slangc.exe")
set(SHADER_SRC_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/shaders_src)
set(SHADER_OUT_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders)

set(SHADERS
    composite_vert
    composite_frag
    tiling
    coverage
)

foreach(SHADER ${SHADERS})
    add_custom_command(
        OUTPUT  ${SHADER_OUT_DIR}/${SHADER}.spv
        COMMAND ${SLANGC} ${SHADER_SRC_DIR}/${SHADER}.slang -o ${SHADER_OUT_DIR}/${SHADER}.spv -target spirv
        DEPENDS ${SHADER_SRC_DIR}/${SHADER}.slang
        COMMENT "Compiling ${SHADER}.slang -> ${SHADER}.spv"
    )
    list(APPEND SPIRV_OUTPUTS ${SHADER_OUT_DIR}/${SHADER}.spv)
endforeach()

add_custom_target(compile_shaders ALL DEPENDS ${SPIRV_OUTPUTS})

add_library(calculator SHARED
    cpp/calculator.cc
    cpp/app.cc
    cpp/renderer.cc
    cpp/ui.cc
    cpp/glyphs.cc
    ${NATIVE_APP_GLUE}/android_native_app_glue.c
)

add_dependencies(calculator compile_shaders)

target_include_directories(calculator PRIVATE
    ${NATIVE_APP_GLUE}
)

target_link_libraries(calculator
    android
    log
    vulkan
)
