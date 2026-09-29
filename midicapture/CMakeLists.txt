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

# --- Dependencies ---
# aubio and libsndfile are found by libaudio (via add_subdirectory below)
# and propagate transitively. We only need Boost here.

# --- Boost program_options (optional, but recommended) ---
# Core Guidelines: find Boost components explicitly.
set(Boost_USE_STATIC_LIBS ON)
find_package(Boost 1.70 REQUIRED COMPONENTS program_options)

# --- libaudio (local static library) ---
# Core Guidelines: link against the local libaudio target. This must run before
# the source list below: it sets the LIBAUDIO_ENABLE_TIER2 cache variable
# (via libaudio's option()), which we read to decide which sources to compile.
add_subdirectory(${CMAKE_SOURCE_DIR}/../libaudio ${CMAKE_BINARY_DIR}/libaudio)

# --- Main executable ---
# Core Guidelines: explicit source files. No wildcards.
set(MIDICAPTURE_SOURCES
    src/main.cpp
)
# Tier-2 analyzers + the analyzer-agnostic 14-file corpus harness. Compiled only
# when libaudio was built with Tier-2, so a Tier-1 build is byte-for-byte
# unaffected (these files are not in its source list at all). Each file's body
# is additionally guarded by LIBAUDIO_HAS_TIER2 (a public compile definition
# libaudio propagates here) so the sources can also be compiled standalone.
if(LIBAUDIO_ENABLE_TIER2)
    list(APPEND MIDICAPTURE_SOURCES
        src/corpusHarness.cpp
    )
endif()

add_executable(midicapture
    ${MIDICAPTURE_SOURCES}
)

# Core Guidelines: public include directory for consumers of the library.
target_include_directories(midicapture PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
)

# Core Guidelines: link against libaudio (which transitively provides
# aubio + libsndfile include dirs and libs) and Boost program_options.
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
