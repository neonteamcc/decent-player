cmake_minimum_required(VERSION 3.22.1)
project("decent_usb_audio" C CXX)

set(CMAKE_CXX_STANDARD 11)

# Size hygiene (applies to libFLAC below too): hide everything that isn't
# an explicit JNIEXPORT from the dynamic symbol table and let the linker
# drop unreferenced sections. Without this the statically-linked libFLAC
# leaked ~250 API symbols into dynsym and nothing could be GC'd.
add_compile_options(-ffunction-sections -fdata-sections -fvisibility=hidden)

# ── libFLAC (reuse xiph/flac source from decoder-flac module) ──
set(FLAC_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../../decent-media3-decoder-flac/src/main/jni")
set(WITH_OGG OFF CACHE BOOL "" FORCE)
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
add_subdirectory("${FLAC_MODULE_DIR}/libflac"
                 "${CMAKE_CURRENT_BINARY_DIR}/libflac"
                 EXCLUDE_FROM_ALL)

# ── Main library: USB audio driver + native audio engine ──
add_library(decent_usb_audio SHARED
    usb-audio-output.cpp
    native-audio-engine.cpp
    decimator.cpp
    resampler.cpp
    "${FLAC_MODULE_DIR}/flac_parser.cc"
)

target_include_directories(decent_usb_audio PRIVATE
    "${FLAC_MODULE_DIR}/include"
    "${FLAC_MODULE_DIR}/libflac/include"
)

find_library(log-lib log)
target_link_libraries(decent_usb_audio
    ${log-lib}
    FLAC
)
target_link_options(decent_usb_audio PRIVATE
    -Wl,--gc-sections
    -Wl,--exclude-libs,ALL
)
