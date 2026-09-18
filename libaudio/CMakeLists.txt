# /**
#  * @file CMakeLists.txt
#  * @brief Build configuration for libaudio — DSP library for audio-to-MIDI
#  *        transcription.
#  *
#  * This CMake configuration builds libaudio as a C++20 static library.
#  * It finds three dependencies via pkg-config:
#  * - aubio: Core DSP (pitch, onsets, beats, notes)
#  * - libsndfile: Audio file I/O (AIFF, WAV, FLAC, etc.)
#  * - rubberband: Optional time-stretching and pitch-shifting
#  *
#  * @section cmake-structure Build Structure
#  *
#  * The library is built as a static library (`libaudio`) with:
#  * - Public headers in `include/libaudio/`
#  * - Implementation files in `src/`
#  * - Dependencies found via pkg-config (aubio, sndfile, rubberband)
#  *
#  * rubberband is optional: if it is not found, the library still builds
#  * but the RubberbandProcessor module is excluded.
#  *
#  * @see lode/libaudio/summary.md — Module overview and API design
#  */

cmake_minimum_required(VERSION 3.20)

project(libaudio
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

# --- Find dependencies via pkg-config ---
# Core Guidelines: find dependencies explicitly. Fail early with clear errors.
find_package(PkgConfig REQUIRED)
pkg_check_modules(AUBIO REQUIRED aubio)
pkg_check_modules(SNDFILE REQUIRED sndfile)

# rubberband is optional: if not found, the module is excluded.
pkg_check_modules(RUBBERBAND rubberband)

# --- libaudio source files ---
# Core Guidelines: explicit source files. No wildcards.
set(LIBAUDIO_SOURCES
    src/audioFile.cpp
    src/fft.cpp
    src/pitch.cpp
    src/onset.cpp
    src/beat.cpp
    src/notes.cpp
    src/spectral.cpp
    src/temporal.cpp
    src/hir.cpp
    src/midiFileWriter.cpp
    src/controlEventExtractor.cpp
    src/scoreBuilder.cpp
    src/velocityEstimator.cpp
    src/noteTrimmer.cpp
)

# rubberband is currently not linkable (pkg-config path mismatch).
# The RubberbandProcessor module is excluded from the build.

# --- libaudio static library target ---
add_library(libaudio STATIC ${LIBAUDIO_SOURCES})

# Core Guidelines: public include directory for consumers of the library.
target_include_directories(libaudio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${AUBIO_INCLUDE_DIRS}
        ${SNDFILE_INCLUDE_DIRS}
        ${RUBBERBAND_INCLUDE_DIRS}
)

# Core Guidelines: link against aubio and libsndfile (required).
target_link_libraries(libaudio
    PUBLIC
        ${AUBIO_LIBRARIES}
        ${SNDFILE_LIBRARIES}
    PRIVATE
        ${AUBIO_LDFLAGS}
        ${SNDFILE_LDFLAGS}
)

# rubberband is excluded from the build (see note above).
# No rubberband-specific compile definitions needed.

# Core Guidelines: use compiler warnings as errors in development builds.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(libaudio PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wno-unused-parameter
    )
endif()

# ============================================================================
# Test harness — exercise the public API using assert()-based tests.
# Core Guidelines: enable_testing() for CTest integration.
# ============================================================================
enable_testing()

add_executable(test_libaudio
    tests/test_main.cpp
)

target_include_directories(test_libaudio PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${SNDFILE_INCLUDE_DIRS}
)

target_link_libraries(test_libaudio PRIVATE
    libaudio
)

add_test(NAME unit_tests COMMAND test_libaudio)
