cmake_minimum_required(VERSION 3.16)
project(PuppetPinOFX LANGUAGES CXX)

# Puppet Pin OFX plug-in for VEGAS Pro.
#
#   cmake -S . -B build -A x64            (Visual Studio)
#   cmake --build build --config Release
#   ctest --test-dir build -C Release
#
# The plug-in ends up in build/PuppetPin.ofx.bundle/Contents/<arch>/PuppetPin.ofx,
# ready to copy into an OFX plug-in folder.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Link the C/C++ runtime statically so the .ofx needs no redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(WIN32)
  set(OFX_ARCH Win64)
elseif(APPLE)
  set(OFX_ARCH MacOS)
else()
  set(OFX_ARCH Linux-x86-64)
endif()
set(BUNDLE_DIR ${CMAKE_BINARY_DIR}/PuppetPin.ofx.bundle)

add_library(PuppetPin MODULE src/PuppetPin.cpp)
target_include_directories(PuppetPin PRIVATE src third_party/openfx/include)
set_target_properties(PuppetPin PROPERTIES
  PREFIX ""
  SUFFIX ".ofx"
  CXX_VISIBILITY_PRESET hidden
  LIBRARY_OUTPUT_DIRECTORY ${BUNDLE_DIR}/Contents/${OFX_ARCH}
  LIBRARY_OUTPUT_DIRECTORY_DEBUG ${BUNDLE_DIR}/Contents/${OFX_ARCH}
  LIBRARY_OUTPUT_DIRECTORY_RELEASE ${BUNDLE_DIR}/Contents/${OFX_ARCH})
if(MSVC)
  target_compile_options(PuppetPin PRIVATE /W3 /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS)
elseif(MINGW)
  target_compile_options(PuppetPin PRIVATE -Wall -Wextra -O2)
  target_link_options(PuppetPin PRIVATE -static -static-libgcc -static-libstdc++)
else()
  target_compile_options(PuppetPin PRIVATE -Wall -Wextra -O2)
  target_link_libraries(PuppetPin PRIVATE ${CMAKE_DL_LIBS})
endif()

option(PUPPETPIN_TESTS "Build the tests" ON)
if(PUPPETPIN_TESTS)
  enable_testing()
  find_package(Threads REQUIRED)

  add_executable(core_test tests/core_test.cpp)
  target_include_directories(core_test PRIVATE src)

  add_executable(mock_host tests/mock_host.cpp)
  target_include_directories(mock_host PRIVATE third_party/openfx/include)
  target_link_libraries(mock_host PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  if(MSVC)
    target_compile_options(core_test PRIVATE /EHsc /O2)
    target_compile_options(mock_host PRIVATE /EHsc)
  endif()

  add_test(NAME core COMMAND core_test)
  add_test(NAME host_with_overlays COMMAND mock_host $<TARGET_FILE:PuppetPin>)
  add_test(NAME host_without_overlays COMMAND mock_host $<TARGET_FILE:PuppetPin> --no-overlays)
endif()

install(DIRECTORY ${BUNDLE_DIR} DESTINATION .)
