# CMakeLists.txt — aiffcapture
# Core Guidelines: use modern CMake, explicit targets, no implicit globals.
# This project targets macOS and links against Core Audio frameworks.

cmake_minimum_required(VERSION 3.20)

# Project declaration: single-language, explicit versioning.
project(aiffcapture
    VERSION 0.1.0
    LANGUAGES CXX
)

# Core Guidelines: enforce C++20 explicitly. No implicit language standards.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Core: no compiler-specific extensions

# Core Guidelines: use position-independent code by default.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Core Guidelines: explicit output directories for clean builds.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Core Audio frameworks (macOS-only) ---
find_library(CORE_AUDIO_FRAMEWORK CoreAudio)
find_library(AUDIO_TOOLBOX_FRAMEWORK AudioToolbox)
find_library(CORE_FOUNDATION_FRAMEWORK CoreFoundation)

# Verify all frameworks were found. Fail early with a clear error.
if(NOT CORE_AUDIO_FRAMEWORK)
    message(FATAL_ERROR "CoreAudio framework not found. This project requires macOS.")
endif()
if(NOT AUDIO_TOOLBOX_FRAMEWORK)
    message(FATAL_ERROR "AudioToolbox framework not found. This project requires macOS.")
endif()
if(NOT CORE_FOUNDATION_FRAMEWORK)
    message(FATAL_ERROR "CoreFoundation framework not found. This project requires macOS.")
endif()

# --- libsndfile (cross-platform audio file I/O) ---
# Used for writing standard AIFF files. libsndfile handles all chunk
# formatting (FORM, COMM, SSND), byte-order conversion, and file
# finalization. Produces files compatible with ffprobe, QuickTime,
# afinfo, and all standard audio tools.
find_library(LIBSNDFILE sndfile)
if(NOT LIBSNDFILE)
    message(FATAL_ERROR "libsndfile not found. Install with: brew install libsndfile")
endif()
find_path(LIBSNDFILE_INCLUDE_DIR sndfile.h
    /opt/homebrew/include
    /usr/local/include
)
if(NOT LIBSNDFILE_INCLUDE_DIR)
    message(FATAL_ERROR "libsndfile headers not found. Install with: brew install libsndfile")
endif()

# --- Header-only interface library for shared types ---
add_library(aiffcapture_headers INTERFACE)
target_include_directories(aiffcapture_headers
    INTERFACE
        ${CMAKE_SOURCE_DIR}/include
)

# --- Main executable ---
add_executable(aiffcapture
    src/main.cpp
    src/device.cpp
    src/recorder.cpp
    src/aiff.cpp
    src/audio_types.cpp
)

# Link against the header library (for include paths).
target_link_libraries(aiffcapture PRIVATE aiffcapture_headers)

# Link against Core Audio frameworks.
target_link_libraries(aiffcapture PRIVATE
    ${CORE_AUDIO_FRAMEWORK}
    ${AUDIO_TOOLBOX_FRAMEWORK}
    ${CORE_FOUNDATION_FRAMEWORK}
)

# Link against libsndfile for AIFF file writing.
target_link_libraries(aiffcapture PRIVATE ${LIBSNDFILE})
target_include_directories(aiffcapture PRIVATE ${LIBSNDFILE_INCLUDE_DIR})

# Core Guidelines: use compiler warnings as errors in development builds.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(aiffcapture PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wconversion
        -Wsign-conversion
        -Wno-unused-parameter  # Suppress for now; remove as we clean up
    )
endif()

# Core Guidelines: position-dependent warnings for macOS-specific code.
target_compile_definitions(aiffcapture PRIVATE
    $<$<PLATFORM_ID:Darwin>:CORE_AUDIO_AVAILABLE>
)