# Windows build of the bridge core and the sbridge CLI (macOS uses Package.swift / scripts/build.sh).
# Toolchain: clang-cl from Visual Studio (the core needs C11 <stdatomic.h> incl. _Atomic double).
#   powershell -ExecutionPolicy Bypass -File scripts\build_win.ps1
# Targets:
#   bridgecore_win  static core with the WASAPI back-ends (capture + exclusive IEC 61937 output)
#   sbridge         CLI: devices / run / file (scripts/regress.sh drives `file`)
#   bridgecore      bridgecore.dll for the tray app (Windows/SurroundBridge, P/Invoke): the same
#                   sources, the public API (bridge.h, bridge_win.h) exported
cmake_minimum_required(VERSION 3.21)
project(SurroundBridge C)

if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(WARNING "Expected clang-cl; MSVC's C11 atomics are experimental")
endif()

# FFmpeg: vendor/ffmpeg/windows-x64 (scripts\build_ffmpeg_win.ps1), else a sibling Surround Player
# checkout's (../music_player), built with the same options.
set(_ff_default "${CMAKE_SOURCE_DIR}/vendor/ffmpeg/windows-x64")
if(NOT EXISTS "${_ff_default}/include/libavformat/avformat.h" AND
   EXISTS "${CMAKE_SOURCE_DIR}/../music_player/vendor/ffmpeg/windows-x64/include/libavformat/avformat.h")
    get_filename_component(_ff_default "${CMAKE_SOURCE_DIR}/../music_player/vendor/ffmpeg/windows-x64" ABSOLUTE)
endif()
set(FFMPEG_PREFIX "${_ff_default}" CACHE PATH "FFmpeg install prefix (scripts/build_ffmpeg_win.ps1)")
if(NOT EXISTS "${FFMPEG_PREFIX}/include/libavfilter/avfilter.h")
    message(FATAL_ERROR "FFmpeg with libavfilter not found in ${FFMPEG_PREFIX}: run scripts\build_ffmpeg_win.ps1")
endif()
message(STATUS "FFmpeg: ${FFMPEG_PREFIX}")

# FFmpeg's MSVC build uses the static CRT (/MT): match it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(ffmpeg INTERFACE)
target_include_directories(ffmpeg INTERFACE "${FFMPEG_PREFIX}/include")
foreach(lib avfilter avformat avcodec swresample swscale avutil)
    find_library(FF_${lib} NAMES lib${lib}.a ${lib} PATHS "${FFMPEG_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
    target_link_libraries(ffmpeg INTERFACE "${FF_${lib}}")
endforeach()
target_link_libraries(ffmpeg INTERFACE bcrypt ole32 user32 advapi32)

set(CORE "${CMAKE_SOURCE_DIR}/core")
set(SB_WARN -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
if(CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    set(SB_WARN /W4 -Wno-unused-parameter -Wno-sign-compare)   # clang-cl's -Wall means -Weverything
endif()

set(CORE_SRCS
    "${CORE}/bridge.c"
    "${CORE}/pc_encode.c"
    "${CORE}/pc_output.c"
    "${CORE}/pc_upmix.c"
    "${CORE}/platform/windows/audio_wasapi.c"
    "${CORE}/platform/windows/capture_wasapi.c"
    "${CORE}/platform/windows/default_output.c")

add_library(bridgecore_win STATIC ${CORE_SRCS})
target_include_directories(bridgecore_win PUBLIC "${CORE}" "${CORE}/include")
target_compile_definitions(bridgecore_win PUBLIC _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
target_compile_options(bridgecore_win PRIVATE ${SB_WARN})
target_link_libraries(bridgecore_win PUBLIC ffmpeg ole32 avrt winmm)

add_executable(sbridge tools/sbridge.c)
target_compile_options(sbridge PRIVATE ${SB_WARN})
target_link_libraries(sbridge PRIVATE bridgecore_win winmm)

add_library(bridgecore SHARED ${CORE_SRCS})
target_include_directories(bridgecore PRIVATE "${CORE}" "${CORE}/include")
target_compile_definitions(bridgecore PRIVATE PC_BUILD_DLL _CRT_SECURE_NO_WARNINGS _WIN32_WINNT=0x0A00)
target_compile_options(bridgecore PRIVATE ${SB_WARN})
target_link_libraries(bridgecore PRIVATE ffmpeg ole32 avrt winmm)
