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
# Kamikaze builds of libfpv (libfpv docs/INTEGRATION.md "Kamikaze builds").
# libfpv.h lays fpv_frame out differently with LIBFPV_KAMIKAZE (12 radio
# channels instead of 10, so everything after them moves), so the mod must be
# built for the kind of libfpv.dll it loads: ON only with a
# -DLIBFPV_KAMIKAZE=ON libfpv. The core refuses a libfpv.dll of the other kind
# (src/fpvapi.cpp). On a kamikaze dev machine (LIBFPV_KAMIKAZE_DEV=1 or true,
# libfpv CLAUDE.md) local builds default to ON; CI never sets the variable.
set(_kamikaze_default OFF)
if("$ENV{LIBFPV_KAMIKAZE_DEV}" STREQUAL "1" OR "$ENV{LIBFPV_KAMIKAZE_DEV}" STREQUAL "true")
  set(_kamikaze_default ON)
endif()
option(GTASA_FPV_KAMIKAZE "Build for a kamikaze libfpv.dll (dev only)" ${_kamikaze_default})

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
  src/damage.cpp
  src/ear.cpp
  src/drone.cpp
  src/effects.cpp
  src/fpvapi.cpp
  src/kamikaze.cpp
  src/render.cpp
  src/pilot.cpp
  src/sfx.cpp
  src/standin.cpp
  src/video.cpp
  src/wind.cpp
  src/world.cpp
  src/log.cpp
  src/patch.cpp
  src/paths.cpp
)
set_target_properties(gtasa_fpv_core PROPERTIES PREFIX "" OUTPUT_NAME "GTASA.FPV.core")
# The mod's version and commit for libfpv's F8 > About tab (src/fpvapi.cpp).
set(GTASA_FPV_BUILD_INFO "${CMAKE_BINARY_DIR}/generated/build_info.h")
add_custom_target(gtasa_fpv_build_info
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${GTASA_FPV_BUILD_INFO} -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${GTASA_FPV_BUILD_INFO}
  COMMENT "Build info for the About tab")
add_dependencies(gtasa_fpv_core gtasa_fpv_build_info)
target_include_directories(gtasa_fpv_core PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(gtasa_fpv_core PRIVATE winmm d3d9)
# The core appends to the log the loader started.
target_compile_definitions(gtasa_fpv_core PRIVATE FPVLOG_APPEND)

foreach(target gtasa_fpv_loader gtasa_fpv_core)
  # docs/renderers: fpv_video_d3d9.h, the video feed's renderer (src/video.cpp)
  target_include_directories(${target} PRIVATE src "${LIBFPV_DIR}/include" "${LIBFPV_DIR}/docs/renderers")
  # libfpv.dll is loaded at runtime; the header is only used for declarations.
  target_compile_definitions(${target} PRIVATE LIBFPV_STATIC)
  if(GTASA_FPV_KAMIKAZE)
    target_compile_definitions(${target} PRIVATE LIBFPV_KAMIKAZE)
  endif()
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_link_options(${target} PRIVATE -static)
endforeach()
