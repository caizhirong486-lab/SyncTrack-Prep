cmake_minimum_required(VERSION 3.21)
project(SyncTrackPrep VERSION 0.1.0)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Build architectures for Mac OS X" FORCE)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Point these at an existing JUCE/Catch2 checkout to avoid a download.
# Leave empty to let FetchContent grab the pinned versions.
set(STP_JUCE_PATH "" CACHE PATH "Path to an existing JUCE source tree")
set(STP_CATCH2_PATH "" CACHE PATH "Path to an existing Catch2 source tree")

include(FetchContent)

if(STP_JUCE_PATH AND EXISTS "${STP_JUCE_PATH}/CMakeLists.txt")
    message(STATUS "Using local JUCE: ${STP_JUCE_PATH}")
    add_subdirectory("${STP_JUCE_PATH}" "${CMAKE_BINARY_DIR}/_deps/juce-build")
else()
    message(STATUS "Local JUCE not found; FetchContent download")
    FetchContent_Declare(
        juce
        URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.8.zip
    )
    FetchContent_MakeAvailable(juce)
endif()

option(STP_COPY_AFTER_BUILD "Copy the built plug-in into the system plug-in folder" ON)

# ---------------------------------------------------------------------------
# NN denoise engines (Live = libDF/DFN3, HQ = ONNX Runtime/MossFormer2).
# Both are optional: without them the plugin builds with Off/Classic only and
# the Live/HQ entries fall back to passthrough (documented in the changelog).
# Layout (gitignored, created by scripts/fetch_models.sh + scripts/build_libdf.sh):
#   third_party/dfn/lib/libdf_universal.a        static libDF (arm64 + x86_64)
#   third_party/dfn/include/df.h                 C API header
#   third_party/dfn/model/DeepFilterNet3_onnx.tar.gz
#   third_party/ort/onnxruntime-osx-universal2-<ver>/   extracted ORT release
# ---------------------------------------------------------------------------
set(STP_DFN_LIB "" CACHE FILEPATH "Path to a static libDF (universal) archive")
set(STP_ORT_ROOT "" CACHE PATH "Path to an extracted onnxruntime release root")

if(NOT STP_DFN_LIB)
    find_file(STP_DFN_LIB_FOUND libdf_universal.a
        PATHS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dfn/lib" NO_DEFAULT_PATH)
    if(STP_DFN_LIB_FOUND)
        set(STP_DFN_LIB "${STP_DFN_LIB_FOUND}")
    endif()
endif()

if(STP_DFN_LIB AND EXISTS "${STP_DFN_LIB}")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dfn/include/df.h")
        message(FATAL_ERROR "third_party/dfn/include/df.h missing (see scripts/build_libdf.sh)")
    endif()
    add_library(stp_libdf STATIC IMPORTED GLOBAL)
    set_target_properties(stp_libdf PROPERTIES IMPORTED_LOCATION "${STP_DFN_LIB}")
    set(STP_HAVE_DFN3 ON)
    message(STATUS "DFN3 (libDF): ${STP_DFN_LIB}")
else()
    set(STP_HAVE_DFN3 OFF)
    message(STATUS "DFN3 (libDF) not found — Live tier passthrough")
endif()

if(NOT STP_ORT_ROOT)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ort/universal/lib/libonnxruntime.dylib")
        set(STP_ORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ort/universal")
    else()
        file(GLOB STP_ORT_GLOB "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ort/onnxruntime-*")
        if(STP_ORT_GLOB)
            list(GET STP_ORT_GLOB 0 STP_ORT_ROOT)
        endif()
    endif()
endif()

if(STP_ORT_ROOT AND EXISTS "${STP_ORT_ROOT}/lib/libonnxruntime.dylib")
    add_library(stp_ort SHARED IMPORTED GLOBAL)
    set_target_properties(stp_ort PROPERTIES IMPORTED_LOCATION
        "${STP_ORT_ROOT}/lib/libonnxruntime.dylib")
    set(STP_ORT_DYLIB "${STP_ORT_ROOT}/lib/libonnxruntime.dylib")
    set(STP_HAVE_MOSSFORMER ON)
    message(STATUS "MossFormer2 (ORT): ${STP_ORT_DYLIB}")
else()
    set(STP_HAVE_MOSSFORMER OFF)
    message(STATUS "ONNX Runtime not found — HQ tier passthrough")
endif()

juce_add_plugin(SyncTrackPrep
    COMPANY_NAME "wosmai"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD ${STP_COPY_AFTER_BUILD}
    PLUGIN_MANUFACTURER_CODE Wosm
    PLUGIN_CODE Stp1
    FORMATS VST3 Standalone
    PRODUCT_NAME "SyncTrack Prep"
)

juce_generate_juce_header(SyncTrackPrep)

function(stp_link_nn_engines TARGET IS_BUNDLE)
    if(STP_HAVE_DFN3)
        target_link_libraries(${TARGET} PRIVATE stp_libdf)
        target_include_directories(${TARGET} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dfn/include")
        target_compile_definitions(${TARGET} PRIVATE STP_ENABLE_DFN3=1)
    endif()
    if(STP_HAVE_MOSSFORMER)
        target_link_libraries(${TARGET} PRIVATE stp_ort)
        target_include_directories(${TARGET} PRIVATE
            "${STP_ORT_ROOT}/include")
        target_compile_definitions(${TARGET} PRIVATE STP_ENABLE_MOSSFORMER=1)
    endif()
    if(NOT IS_BUNDLE)
        if(STP_HAVE_MOSSFORMER)
            add_custom_command(TARGET ${TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${STP_ORT_DYLIB}" "$<TARGET_FILE_DIR:${TARGET}>/")
            set_target_properties(${TARGET} PROPERTIES
                BUILD_RPATH "@loader_path"
                INSTALL_RPATH "@loader_path")
        endif()
        return()
    endif()
    # The plug-in meta target only builds shared code; the real bundle links
    # happen in the _VST3 / _Standalone sub-targets. Ship the ORT runtime
    # (Frameworks) and the NN models (Resources) into each bundle.
    foreach(ART ${TARGET}_VST3 ${TARGET}_Standalone)
        if(TARGET ${ART})
            if(STP_HAVE_MOSSFORMER)
                add_custom_command(TARGET ${ART} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:${ART}>/../Frameworks"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${STP_ORT_DYLIB}" "$<TARGET_FILE_DIR:${ART}>/../Frameworks/libonnxruntime.dylib")
                set_target_properties(${ART} PROPERTIES
                    BUILD_RPATH "@loader_path/../Frameworks"
                    INSTALL_RPATH "@loader_path/../Frameworks")
            endif()
            if(STP_HAVE_DFN3 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dfn/model/DeepFilterNet3_onnx.tar.gz")
                add_custom_command(TARGET ${ART} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:${ART}>/../Resources/dfn3"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dfn/model/DeepFilterNet3_onnx.tar.gz"
                        "$<TARGET_FILE_DIR:${ART}>/../Resources/dfn3/")
            endif()
            if(STP_HAVE_MOSSFORMER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mossformer2/mossformer2_fp32.onnx")
                add_custom_command(TARGET ${ART} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        "$<TARGET_FILE_DIR:${ART}>/../Resources/mossformer2"
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mossformer2/mossformer2_fp32.onnx"
                        "$<TARGET_FILE_DIR:${ART}>/../Resources/mossformer2/")
            endif()
        endif()
    endforeach()
endfunction()

set(STP_DSP_SOURCES
    src/dsp/ChannelRepair.cpp
    src/dsp/PeakCompressor.cpp
    src/dsp/ToneShaper.cpp
    src/dsp/TruePeakLimiter.cpp
    src/dsp/Leveler.cpp
    src/dsp/NoiseSuppressor.cpp
    src/dsp/UpwardExpander.cpp
    src/dsp/Dfn3Denoise.cpp
    src/dsp/MossFormerDenoise.cpp
)

target_sources(SyncTrackPrep
    PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
        ${STP_DSP_SOURCES}
)

target_include_directories(SyncTrackPrep PRIVATE src)

target_compile_definitions(SyncTrackPrep
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

target_link_libraries(SyncTrackPrep
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

stp_link_nn_engines(SyncTrackPrep ON)

# Offline render tool (no DAW) — always available for A/B reference
add_executable(SyncTrackPrepOffline
    tools/offline_render.cpp
    ${STP_DSP_SOURCES}
)
target_include_directories(SyncTrackPrepOffline PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(SyncTrackPrepOffline PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
)
target_link_libraries(SyncTrackPrepOffline PRIVATE
    juce::juce_dsp
    juce::juce_audio_formats
    juce::juce_audio_basics
    juce::juce_core
    juce::juce_recommended_config_flags
)

stp_link_nn_engines(SyncTrackPrepOffline OFF)

option(STP_BUILD_TESTS "Build SyncTrack Prep unit tests" ON)

if(STP_BUILD_TESTS)
    enable_testing()

    if(STP_CATCH2_PATH AND EXISTS "${STP_CATCH2_PATH}/CMakeLists.txt")
        message(STATUS "Using local Catch2: ${STP_CATCH2_PATH}")
        add_subdirectory("${STP_CATCH2_PATH}" "${CMAKE_BINARY_DIR}/_deps/catch2-build")
        set(Catch2_SOURCE_DIR "${STP_CATCH2_PATH}")
    else()
        message(STATUS "Local Catch2 not found; FetchContent download")
        FetchContent_Declare(
            Catch2
            URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.4.0.zip
        )
        FetchContent_MakeAvailable(Catch2)
    endif()

    add_executable(SyncTrackPrepTests
        tests/test_channel_repair.cpp
        tests/test_peak_compressor.cpp
        tests/test_tone_shaper.cpp
        tests/test_true_peak_limiter.cpp
        tests/test_true_peak.cpp
        tests/test_leveler.cpp
        tests/test_noise_suppressor.cpp
        tests/test_presets.cpp
        tests/test_block_invariance.cpp
        tests/test_offline_gold.cpp
        tests/test_upward_expander.cpp
        tests/test_classic_denoise_regression.cpp
        tests/test_dfn3_denoise.cpp
        tests/test_mossformer_frontend.cpp
        tests/test_stage_resampler.cpp
        ${STP_DSP_SOURCES}
    )

    target_include_directories(SyncTrackPrepTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/SyncTrackPrep_artefacts/JuceLibraryCode
    )

    target_compile_definitions(SyncTrackPrepTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
    )

    target_link_libraries(SyncTrackPrepTests
        PRIVATE
            Catch2::Catch2WithMain
            juce::juce_dsp
            juce::juce_audio_basics
            juce::juce_audio_formats
            juce::juce_core
            juce::juce_recommended_config_flags
    )

    stp_link_nn_engines(SyncTrackPrepTests OFF)

    include(CTest)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
    include(Catch)
    catch_discover_tests(SyncTrackPrepTests)

    # Processor-level tests need the plugin shared-code target (APVTS, state,
    # JucePlugin_* definitions), which the DSP-only test binary does not link.
    add_executable(SyncTrackPrepStateTests
        tests/test_processor_state.cpp
    )

    target_include_directories(SyncTrackPrepStateTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_BINARY_DIR}/SyncTrackPrep_artefacts/JuceLibraryCode
    )

    target_link_libraries(SyncTrackPrepStateTests
        PRIVATE
            Catch2::Catch2WithMain
            SyncTrackPrep
            juce::juce_audio_utils
            juce::juce_dsp
            juce::juce_recommended_config_flags
    )

    stp_link_nn_engines(SyncTrackPrepStateTests ON)

    catch_discover_tests(SyncTrackPrepStateTests)
endif()
