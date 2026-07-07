cmake_minimum_required(VERSION 3.22.1)
project(calculator)

# ── Paths into the engine submodules ────────────────────────────────────────
set(REPO_ROOT     ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(CANVAS_ENGINE ${REPO_ROOT}/libs/vulkan_canvas_engine)
set(CANVAS_CPP    ${CANVAS_ENGINE}/app/src/main/cpp)
set(FONT_ENGINE   ${CANVAS_ENGINE}/vulkan_font_engine)
set(FONT_CPP      ${FONT_ENGINE}/app/src/main/cpp)
set(FONT_SHADERS  ${FONT_ENGINE}/app/src/main/shaders_src)
set(FREETYPE_DIR  ${FONT_ENGINE}/app/src/main/freetype)

set(CORE_DIR ${REPO_ROOT}/core)

set(NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)

# ── FreeType (vendored in the font engine submodule) ────────────────────────
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
add_subdirectory(${FREETYPE_DIR} ${CMAKE_BINARY_DIR}/freetype)
add_library(Freetype::Freetype ALIAS freetype)

# ── msdfgen (vendored in the font engine, next to FreeType) ─────────────────
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_SVG ON CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(MSDFGEN_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${FONT_ENGINE}/app/src/main/msdfgen ${CMAKE_BINARY_DIR}/msdfgen)

# ── Shader compilation (Slang -> SPIR-V) via the engine's reusable helper ────
# Prefer slangc on PATH; otherwise VceShaders.cmake falls back to the Vulkan SDK
# default. Override with -DVCE_SLANGC=/path/to/slangc.
find_program(VCE_SLANGC_FOUND slangc)
if(VCE_SLANGC_FOUND)
    set(VCE_SLANGC ${VCE_SLANGC_FOUND} CACHE FILEPATH "Slang compiler" FORCE)
endif()
include(${CANVAS_ENGINE}/cmake/VceShaders.cmake)

set(SHADER_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders)
vce_compile_slang(app_shaders ${SHADER_OUT_DIR} ${FONT_SHADERS}
    tiling
    coverage
    overlay_vert
    overlay_frag
    msdf_vert
    msdf_frag
)

# ── Native library (name must match android.app.lib_name = "calculator") ─────
add_library(calculator SHARED
    cpp/main.cc
    cpp/mathbox.cc
    cpp/mathlayout.cc
    cpp/saf_bridge.cc
    ${CORE_DIR}/math/lexer.cc
    ${CORE_DIR}/math/parser.cc
    ${CORE_DIR}/math/numeric_evaluator.cc
    ${CORE_DIR}/calc/calc.cc
    ${CORE_DIR}/calc/editor.cc
    ${CORE_DIR}/graphing/plot.cc
    ${CORE_DIR}/cas/symbolic_engine.cc
    ${CORE_DIR}/cas/cas_worker.cc
    ${CORE_DIR}/cas/eigenmath_engine.cc
    ${CORE_DIR}/cas/eigenmath/eigenmath.c
    ${CORE_DIR}/db/store.cc
    ${CORE_DIR}/db/sqlite3.c
    ${CANVAS_CPP}/canvas.cc
    ${CANVAS_CPP}/overlay.cc
    ${CANVAS_CPP}/canvas_host.cc
    ${CANVAS_CPP}/gesture.cc
    ${CANVAS_CPP}/pager.cc
    ${CANVAS_CPP}/plotview.cc
    ${FONT_CPP}/font.cc
    ${FONT_CPP}/glyphs.cc
    ${FONT_CPP}/msdf.cc
    ${NATIVE_APP_GLUE}/android_native_app_glue.c
)

add_dependencies(calculator app_shaders)

target_include_directories(calculator PRIVATE
    ${NATIVE_APP_GLUE}
    ${CANVAS_CPP}
    ${FONT_CPP}
    ${FREETYPE_DIR}/include
    ${CORE_DIR}
    ${FONT_ENGINE}/app/src/main/msdfgen
    ${CMAKE_BINARY_DIR}
)

target_compile_options(calculator PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
    -O2
    -fno-fast-math -ffp-contract=off  # correctness pin: this is a calculator —
    # NumericEvaluator/Eigenmath depend on strict IEEE-754 (real NaN/Inf, no
    # reassociation, no fused-multiply-add rounding drift). Never add -Ofast or
    # -ffast-math here even for a speed win.
)

# Vendored SQLite (core/db/sqlite3.c): trim unused features; serialized threading
# so the synchronous startup read and the background writer share one connection
# safely. These macros only affect the SQLite TU.
target_compile_definitions(calculator PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DQS=0
    CAS_USE_EIGENMATH        # selects the Eigenmath CasEngine (else the dyno)
)

# Vendored Eigenmath (core/cas/eigenmath/eigenmath.c): third-party C, compiled
# as-is — silence its warnings so they don't drown our own build output.
set_source_files_properties(${CORE_DIR}/cas/eigenmath/eigenmath.c
    PROPERTIES COMPILE_OPTIONS "-w")

# msdfgen-core is linked EXPLICITLY (msdfgen-ext links it only PRIVATE): its
# PUBLIC `MSDFGEN_PUBLIC=` define makes msdfgen/core/base.h skip the generated
# msdfgen-config.h, which only exists under MSDFGEN_INSTALL — without it the
# header had to be hand-faked here and its bare defines clashed with the
# command-line `-DMSDFGEN_*` (macro-redefined warnings in every msdfgen TU).
target_link_libraries(calculator
    freetype
    msdfgen::msdfgen-ext
    msdfgen::msdfgen-core
    android
    log
    vulkan
)
