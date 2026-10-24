# Cross-compiles libfpv for 32-bit Windows (x86, e.g. GTA San Andreas) from Linux with MinGW-w64.
# The "-posix" compilers provide std::thread (winpthreads); everything is
# linked statically, so libfpv.dll has no runtime dependencies.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)
set(TOOLCHAIN_PREFIX i686-w64-mingw32)
find_program(CMAKE_C_COMPILER NAMES ${TOOLCHAIN_PREFIX}-gcc-posix ${TOOLCHAIN_PREFIX}-gcc REQUIRED)
find_program(CMAKE_CXX_COMPILER NAMES ${TOOLCHAIN_PREFIX}-g++-posix ${TOOLCHAIN_PREFIX}-g++ REQUIRED)
find_program(CMAKE_RC_COMPILER NAMES ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Every binary links the GCC, C++ and winpthreads runtimes statically:
# without this, executables need libgcc_s_dw2-1.dll, libstdc++-6.dll and
# libwinpthread-1.dll next to them (and fail to start silently otherwise).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++ -static")

# i686 GCC defaults to x87 floating point, whose 80-bit intermediates change
# the flight and collision maths enough to fail tests (a 63 m/s drone
# tunnelled through a wall). SSE2 matches the x86_64 results; every CPU that
# runs a 32-bit host game such as GTA SA has it.
set(CMAKE_C_FLAGS_INIT "-msse2 -mfpmath=sse")
set(CMAKE_CXX_FLAGS_INIT "-msse2 -mfpmath=sse")
