# /**
#  * @file CMakeLists.txt
#  * @brief Build configuration for waterfall — audio frequency analysis.
#  *
#  * This CMake configuration builds the waterfall executable. It reads an
#  * audio file and renders a frequency analysis (a SONAR-style waterfall) of
#  * its acoustic energy over time. It links:
#  * - libaudio (static library): DSP, FFT/spectral analysis, audio I/O
#  * - Boost program_options: command-line parsing
#  *
#  * @section cmake-dependencies Dependencies
#  *
#  * Required packages (found via pkg-config):
#  * - aubio: Core DSP (FFT, spectral analysis, onsets)
#  * - libsndfile: Audio file I/O
#  *
#  * Optional packages:
#  * - Boost program_options: Command-line argument parsing
#  *
#  */

cmake_minimum_required(VERSION 3.20)

project(waterfall
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
# Core Guidelines: link against the local libaudio target.
add_subdirectory(${CMAKE_SOURCE_DIR}/../libaudio ${CMAKE_BINARY_DIR}/libaudio)

# --- Main executable ---
# Core Guidelines: explicit source files. No wildcards.
add_executable(waterfall
    src/main.cpp
)

# Core Guidelines: include directories and link libraries.
target_include_directories(waterfall PRIVATE
    ${Boost_INCLUDE_DIRS}
)

# libaudio transitively provides aubio + libsndfile include dirs and libs.
target_link_libraries(waterfall PRIVATE
    libaudio
    ${Boost_LIBRARIES}
)

# Core Guidelines: use compiler warnings as errors in development builds.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(waterfall PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wno-unused-parameter
        # Boost headers emit unused-pragma warnings under -Wpedantic;
        # they are not actionable in user code.
        -Wno-pragma-clang-attribute
        -Werror
    )
endif()
