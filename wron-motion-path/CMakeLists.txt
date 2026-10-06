cmake_minimum_required(VERSION 3.16)
project(WronMotionPath VERSION 0.1.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Build flavour. Only a developer build exists today: the Wron licence module
# lives in the (missing) Wron source tree and must be linked in before any
# customer build can be produced. A non-dev configure fails on purpose so a
# build without licensing can never be mistaken for a release.
# ---------------------------------------------------------------------------
option(WMP_DEV_BUILD "Developer build (not for distribution, no licence check)" ON)
set(WMP_LICENSE_MODULE_DIR "" CACHE PATH "Path to the existing Wron licence module (required for customer builds)")
if(NOT WMP_DEV_BUILD)
  if(WMP_LICENSE_MODULE_DIR STREQUAL "")
    message(FATAL_ERROR
      "Customer build requested but WMP_LICENSE_MODULE_DIR is not set. "
      "Wire in the existing Wron licence validation first; it must not be removed or bypassed.")
  endif()
  message(FATAL_ERROR "Licence integration is not implemented yet: the Wron licence sources are not in this repository.")
endif()

option(WMP_BUILD_TESTS "Build unit tests and the mock OFX host" ON)
option(WMP_BUILD_OFX "Build the OFX plug-in bundle" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(NOMINMAX _USE_MATH_DEFINES)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

# --------------------------- core library ----------------------------------
add_library(wmp_core STATIC
  core/src/json.cpp
  core/src/path.cpp
  core/src/document.cpp
  core/src/arclength.cpp
  core/src/geometry.cpp
  core/src/timing.cpp
  core/src/motion.cpp
  core/src/render.cpp
  core/src/pipeline.cpp
  core/src/presets.cpp
  core/src/retime.cpp
  core/src/cache.cpp
  core/src/edit.cpp
)
target_include_directories(wmp_core PUBLIC core/include)
find_package(Threads REQUIRED)
target_link_libraries(wmp_core PUBLIC Threads::Threads)

# ----------------------------- OFX plug-in ---------------------------------
# Bundle layout (OpenFX): WronMotionPath.ofx.bundle/Contents/<arch>/WronMotionPath.ofx
#   Windows x64: Contents/Win64   (VEGAS Pro 2026 target; build with MSVC)
#   Linux x86-64: Contents/Linux-x86-64  (developer/mock-host testing only)
if(WMP_BUILD_OFX)
  if(WIN32)
    set(WMP_OFX_ARCH "Win64")
  elseif(APPLE)
    set(WMP_OFX_ARCH "MacOS")
  else()
    set(WMP_OFX_ARCH "Linux-x86-64")
  endif()
  set(WMP_BUNDLE_DIR "${CMAKE_BINARY_DIR}/WronMotionPath.ofx.bundle")

  add_library(WronMotionPath MODULE
    ofx/src/plugin.cpp
    ofx/src/overlay.cpp
    ofx/src/ofx_util.cpp
  )
  target_include_directories(WronMotionPath PRIVATE ofx/src ofx/third_party/openfx-1.5.1/include)
  target_link_libraries(WronMotionPath PRIVATE wmp_core)
  target_compile_definitions(WronMotionPath PRIVATE WMP_DEV_BUILD=1)
  set_target_properties(WronMotionPath PROPERTIES
    PREFIX ""
    SUFFIX ".ofx"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${WMP_BUNDLE_DIR}/Contents/${WMP_OFX_ARCH}"
    RUNTIME_OUTPUT_DIRECTORY "${WMP_BUNDLE_DIR}/Contents/${WMP_OFX_ARCH}")
  if(MSVC)
    # Static CRT: the bundle needs no extra runtime DLLs next to VEGAS.
    set_property(TARGET WronMotionPath wmp_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  elseif(MINGW)
    target_link_options(WronMotionPath PRIVATE -static -static-libgcc -static-libstdc++)
  endif()

  # Overlay drawing: OFX 1.5 Draw Suite needs nothing; the legacy OpenGL path
  # (OverlayInteractV1) is compiled when OpenGL is available (always on Windows).
  find_package(OpenGL QUIET)
  if(OpenGL_FOUND OR WIN32)
    target_compile_definitions(WronMotionPath PRIVATE WMP_OVERLAY_OPENGL=1)
    if(WIN32)
      target_link_libraries(WronMotionPath PRIVATE opengl32)
    else()
      target_link_libraries(WronMotionPath PRIVATE OpenGL::GL)
    endif()
  endif()
endif()

# ------------------------------- tests -------------------------------------
if(WMP_BUILD_TESTS)
  enable_testing()
  add_executable(wmp_core_tests
    core/tests/test_main.cpp
    core/tests/test_json_document.cpp
    core/tests/test_geometry.cpp
    core/tests/test_timing_motion.cpp
    core/tests/test_render.cpp
    core/tests/test_presets_retime_cache.cpp
    core/tests/test_edit.cpp
  )
  target_link_libraries(wmp_core_tests PRIVATE wmp_core)
  add_test(NAME core COMMAND wmp_core_tests)

  if(WMP_BUILD_OFX AND NOT WIN32)
    # End-to-end tests of the real .ofx through a mock OFX host (not VEGAS).
    add_executable(wmp_ofx_host_tests core/tests/test_main.cpp ofx/tests/mock_host.cpp ofx/tests/test_ofx_host.cpp)
    target_include_directories(wmp_ofx_host_tests PRIVATE ofx/third_party/openfx-1.5.1/include)
    target_link_libraries(wmp_ofx_host_tests PRIVATE wmp_core ${CMAKE_DL_LIBS})
    target_compile_definitions(wmp_ofx_host_tests PRIVATE WMP_PLUGIN_PATH="$<TARGET_FILE:WronMotionPath>")
    add_dependencies(wmp_ofx_host_tests WronMotionPath)
    add_test(NAME ofx_host COMMAND wmp_ofx_host_tests)

    # Legacy OpenGL overlay path drawn into an off-screen OSMesa context.
    find_library(OSMESA_LIBRARY OSMesa)
    if(OSMESA_LIBRARY)
      add_executable(wmp_ofx_gl_overlay_test core/tests/test_main.cpp ofx/tests/mock_host.cpp ofx/tests/test_overlay_gl.cpp)
      target_include_directories(wmp_ofx_gl_overlay_test PRIVATE ofx/third_party/openfx-1.5.1/include)
      # OSMesa first so the plug-in's gl* calls bind to the off-screen implementation.
      target_link_libraries(wmp_ofx_gl_overlay_test PRIVATE ${OSMESA_LIBRARY} wmp_core ${CMAKE_DL_LIBS})
      target_compile_definitions(wmp_ofx_gl_overlay_test PRIVATE WMP_PLUGIN_PATH="$<TARGET_FILE:WronMotionPath>")
      add_dependencies(wmp_ofx_gl_overlay_test WronMotionPath)
      add_test(NAME ofx_gl_overlay COMMAND wmp_ofx_gl_overlay_test)
    endif()
  endif()

  add_executable(wmp_bench tools/bench_render.cpp)
  target_link_libraries(wmp_bench PRIVATE wmp_core)
endif()
