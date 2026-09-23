# /**
#  * @file CMakeLists.txt
#  * @brief Build configuration for midicapture — audio-to-MIDI transcription.
#  *
#  * This CMake configuration builds the midicapture executable. It links:
#  * - libaudio (static library): DSP, pitch detection, MIDI writing
#  * - Boost program_options: command-line parsing
#  *
#  * @section cmake-dependencies Dependencies
#  *
#  * Required packages (found via pkg-config):
#  * - aubio: Core DSP (pitch, onsets, beats, notes)
#  * - libsndfile: Audio file I/O
#  *
#  * Optional packages:
#  * - Boost program_options: Command-line argument parsing
#  *
#  */

cmake_minimum_required(VERSION 3.20)

project(midicapture
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Core: no compiler-specific extensions

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# --- Find dependencies via pkg-config ---
# Core Guidelines: find dependencies explicitly. Fail early with clear errors.
find_package(PkgConfig REQUIRED)
pkg_check_modules(AUBIO REQUIRED aubio)
pkg_check_modules(SNDFILE REQUIRED sndfile)

# --- Boost program_options (optional, but recommended) ---
# Core Guidelines: find Boost components explicitly.
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.70 REQUIRED COMPONENTS program_options)

# --- libaudio (local static library) ---
# Core Guidelines: link against the local libaudio target.
add_subdirectory(${CMAKE_SOURCE_DIR}/../libaudio ${CMAKE_BINARY_DIR}/libaudio)

# --- Main executable ---
# Core Guidelines: explicit source files. No wildcards.
add_executable(midicapture
    src/main.cpp
    src/transcriber.cpp
    src/audioFile.cpp
)

# Core Guidelines: public include directory for consumers of the library.
target_include_directories(midicapture PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${AUBIO_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
)

# Core Guidelines: link against libaudio (which transitively links aubio
# and libsndfile) and Boost program_options.
target_link_libraries(midicapture PRIVATE
    libaudio
    ${Boost_LIBRARIES}
)

# Core Guidelines: use compiler warnings as errors in development builds.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(midicapture PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wno-pragma-clang-attribute
        -Werror
        -Wno-unused-parameter
    )
endif()
