cmake_minimum_required(VERSION 3.21.0 FATAL_ERROR)
project(flowyaudio C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# This is a file-conversion path, never the audio hot path — size beats speed,
# exactly as in build-ffmpeg.sh. -ffunction-sections/-fdata-sections are only
# worth anything with --gc-sections, so the two travel together.
add_compile_options(-Os -ffunction-sections -fdata-sections -fvisibility=hidden)
add_link_options(-Wl,--gc-sections)

# Android 15+ devices may use 16 KB memory pages, and the four FFmpeg
# libraries this wrapper sits beside are already linked that way. A wrapper
# without the same alignment fails to load next to them.
add_link_options(-Wl,-z,max-page-size=16384)

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(PREBUILT "${ROOT}/prebuilt/${ANDROID_ABI}/lib")

if(NOT EXISTS "${PREBUILT}/libavcodec.so")
  message(FATAL_ERROR
    "prebuilt/${ANDROID_ABI}/lib/libavcodec.so is missing. "
    "Run `bash setup.sh && bash build-ffmpeg.sh` in the module root first.")
endif()

# One shared include tree for all four ABIs; build-ffmpeg.sh asserts the
# headers are identical across them before publishing it.
include_directories("${ROOT}/prebuilt/include")

# Linked by explicit path, never through pkg-config: build-ffmpeg.sh deletes
# lib/pkgconfig/ on purpose, because the .pc files record an absolute build
# path in a public artifact.
foreach(lib avcodec avformat avutil swresample)
  add_library(${lib} SHARED IMPORTED)
  set_target_properties(${lib} PROPERTIES
    IMPORTED_LOCATION "${PREBUILT}/lib${lib}.so")
endforeach()

add_library(flowyaudio SHARED flowy_audio_jni.cc)

find_library(android_log_lib log)
target_link_libraries(flowyaudio avformat avcodec swresample avutil ${android_log_lib})
