cmake_minimum_required(VERSION 3.16)
project(AIMotionNative LANGUAGES CXX)

# puppet_render: Puppet Pin preview renderer for the AI Motion tool. It
# compiles PuppetCore.h from the Puppet Pin OFX plug-in, so previews match
# what the effect renders in VEGAS.
#
#   cmake -S . -B build -A x64
#   cmake --build build --config Release

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(PUPPET_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../PuppetPinOFX/src)

add_executable(puppet_render puppet_render.cpp)
target_include_directories(puppet_render PRIVATE ${PUPPET_SRC})
if(MSVC)
  target_compile_options(puppet_render PRIVATE /W3 /EHsc /O2 /D_CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(puppet_render PRIVATE -Wall -Wextra -O2)
endif()
