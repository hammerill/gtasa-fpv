cmake_minimum_required(VERSION 3.24)
project(gtasa_fpv CXX)

# Build with: cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "GTA San Andreas is 32-bit: configure with cmake/mingw-w64-i686.cmake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(LIBFPV_DIR "$ENV{HOME}/Projects/libfpv" CACHE PATH "libfpv source checkout")
set(LIBFPV_BUILD "${LIBFPV_DIR}/build/win32" CACHE PATH "libfpv i686 build dir")

# GTASA.FPV.asi: loader that hooks the game loop and hot-reloads the core.
add_library(gtasa_fpv_loader SHARED
  src/loader.cpp
  src/log.cpp
  src/patch.cpp
  src/paths.cpp
)
set_target_properties(gtasa_fpv_loader PROPERTIES PREFIX "" OUTPUT_NAME "GTASA.FPV" SUFFIX ".asi")

# GTASA.FPV.core.dll: the mod itself.
add_library(gtasa_fpv_core SHARED
  src/core.cpp
  src/audio.cpp
  src/autotest.cpp
  src/drone.cpp
  src/effects.cpp
  src/fpvapi.cpp
  src/kamikaze.cpp
  src/render.cpp
  src/pilot.cpp
  src/sfx.cpp
  src/standin.cpp
  src/world.cpp
  src/log.cpp
  src/patch.cpp
  src/paths.cpp
)
set_target_properties(gtasa_fpv_core PROPERTIES PREFIX "" OUTPUT_NAME "GTASA.FPV.core")
target_link_libraries(gtasa_fpv_core PRIVATE winmm d3d9)
# The core appends to the log the loader started.
target_compile_definitions(gtasa_fpv_core PRIVATE FPVLOG_APPEND)

foreach(target gtasa_fpv_loader gtasa_fpv_core)
  target_include_directories(${target} PRIVATE src "${LIBFPV_DIR}/include")
  # libfpv.dll is loaded at runtime; the header is only used for declarations.
  # Kamikaze declarations too: the core finds out at runtime whether the
  # loaded libfpv.dll has them (src/fpvapi.cpp), so either build works.
  target_compile_definitions(${target} PRIVATE LIBFPV_STATIC LIBFPV_KAMIKAZE)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_options(${target} PRIVATE -static)
endforeach()
