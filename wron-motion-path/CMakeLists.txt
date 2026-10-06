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
)
target_include_directories(wmp_core PUBLIC core/include)
find_package(Threads REQUIRED)
target_link_libraries(wmp_core PUBLIC Threads::Threads)

# ------------------------------- tests -------------------------------------
if(WMP_BUILD_TESTS)
  enable_testing()
  add_executable(wmp_core_tests
    core/tests/test_main.cpp
    core/tests/test_json_document.cpp
    core/tests/test_geometry.cpp
    core/tests/test_timing_motion.cpp
  )
  target_link_libraries(wmp_core_tests PRIVATE wmp_core)
  add_test(NAME core COMMAND wmp_core_tests)
endif()
