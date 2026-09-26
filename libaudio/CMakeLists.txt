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
pkg_check_modules(AUBIO REQUIRED aubio IMPORTED_TARGET)
pkg_check_modules(SNDFILE REQUIRED sndfile IMPORTED_TARGET)

# rubberband is optional: if not found, the module is excluded.
pkg_check_modules(RUBBERBAND rubberband IMPORTED_TARGET)

# --- Tier 2 (optional) -------------------------------------------------------
# Neural transcription on ONNX Runtime + Core ML EP. OFF by default so the aubio
# Tier-1 path is byte-for-byte unaffected. Requires `brew install onnxruntime`.
option(LIBAUDIO_ENABLE_TIER2
    "Enable Tier-2 neural transcription (ONNX Runtime + Core ML EP)"
    OFF)

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
# Use CMAKE_CURRENT_SOURCE_DIR so it works both as standalone and subdirectory.
target_include_directories(libaudio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        $<IF:$<BOOL:${AUBIO_FOUND}>,$<TARGET_PROPERTY:PkgConfig::AUBIO,INCLUDE_DIRECTORY>,>
        $<IF:$<BOOL:${SNDFILE_FOUND}>,$<TARGET_PROPERTY:PkgConfig::SNDFILE,INCLUDE_DIRECTORY>,>
        $<IF:$<BOOL:${RUBBERBAND_FOUND}>,$<TARGET_PROPERTY:PkgConfig::RUBBERBAND,INCLUDE_DIRECTORY>,>
)

# Core Guidelines: link against aubio and libsndfile (required).
# Using CMake imported targets (PkgConfig::*) so that CMake deduplicates
# -L paths and -l flags automatically (avoids Apple ld duplicate-library
# warnings that raw pkg-config flags would produce).
if(AUBIO_FOUND)
    target_link_libraries(libaudio PUBLIC PkgConfig::AUBIO)
endif()
if(SNDFILE_FOUND)
    target_link_libraries(libaudio PUBLIC PkgConfig::SNDFILE)
endif()

# rubberband is excluded from the build (see note above).
# No rubberband-specific compile definitions needed.

# ============================================================================
# Tier 2 — optional neural transcription via ONNX Runtime.
# ============================================================================
if(LIBAUDIO_ENABLE_TIER2)
    # (b) Find onnxruntime via its installed CMake config, against the Homebrew
    #     prefix. `brew install onnxruntime` places the config package at
    #     /opt/homebrew/opt/onnxruntime/lib/cmake/onnxruntime.
    list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew/opt/onnxruntime")
    find_package(onnxruntime CONFIG REQUIRED)

    # The onnxruntime config sets its include dir to .../include/onnxruntime
    # (Homebrew) or .../include (official builds). Our sources use the canonical
    # <onnxruntime/...> include prefix, so also put the *parent* of that dir on
    # the path — this makes the prefix resolve under either layout.
    get_target_property(_onnx_inc onnxruntime::onnxruntime INTERFACE_INCLUDE_DIRECTORIES)
    get_filename_component(_onnx_inc_parent "${_onnx_inc}" DIRECTORY)
    if(NOT _onnx_inc_parent STREQUAL "")
        target_include_directories(libaudio PUBLIC "${_onnx_inc_parent}")
    endif()

    target_sources(libaudio PRIVATE
        src/tier2/onnxTensor.cpp
        src/tier2/onnxSession.cpp
        src/tier2/modelDescriptor.cpp
    )

    # PUBLIC so consumers (midicapture/waterfall) that use the Tier-2 types
    # transitively link the onnxruntime dylib and see the umbrella-header guard.
    target_compile_definitions(libaudio PUBLIC LIBAUDIO_HAS_TIER2=1)
    target_link_libraries(libaudio PUBLIC onnxruntime::onnxruntime)

    # The basic-pitch model is referenced in place from the submodule (no copy):
    # a single source of truth, with provenance in manifests/basic-pitch.txt.
    set(LIBAUDIO_BASICPITCH_MODEL
        "${CMAKE_CURRENT_SOURCE_DIR}/external/basic-pitch/basic_pitch/saved_models/icassp_2022/nmp.onnx")
    target_compile_definitions(libaudio PUBLIC
        LIBAUDIO_BASICPITCH_MODEL="${LIBAUDIO_BASICPITCH_MODEL}")
    message(STATUS "libaudio: Tier-2 (ONNX) ENABLED — basic-pitch model: ${LIBAUDIO_BASICPITCH_MODEL}")
else()
    message(STATUS "libaudio: Tier-2 (ONNX) disabled (LIBAUDIO_ENABLE_TIER2=OFF)")
endif()

# Core Guidelines: use compiler warnings as errors in development builds.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(libaudio PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wno-unused-parameter
        -Werror
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
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${SNDFILE_INCLUDE_DIRS}
)

target_link_libraries(test_libaudio PRIVATE
    libaudio
)

add_test(NAME unit_tests COMMAND test_libaudio)

# ============================================================================
# Tier-2 smoke test — prove the C++/ONNX/CoreML path loads + runs the model.
# ============================================================================
if(LIBAUDIO_ENABLE_TIER2)
    add_executable(test_libaudio_tier2 tests/tier2_smoke.cpp)
    target_include_directories(test_libaudio_tier2 PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(test_libaudio_tier2 PRIVATE libaudio)
    add_test(NAME tier2_smoke COMMAND test_libaudio_tier2)
endif()