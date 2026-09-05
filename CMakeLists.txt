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

target_sources(SyncTrackPrep
    PRIVATE
        src/PluginProcessor.cpp
        src/PluginEditor.cpp
        src/dsp/ChannelRepair.cpp
        src/dsp/PeakCompressor.cpp
        src/dsp/ToneShaper.cpp
        src/dsp/TruePeakLimiter.cpp
        src/dsp/Leveler.cpp
        src/dsp/NoiseSuppressor.cpp
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

# Offline render tool (no DAW) — always available for A/B reference
add_executable(SyncTrackPrepOffline
    tools/offline_render.cpp
    src/dsp/ChannelRepair.cpp
    src/dsp/PeakCompressor.cpp
    src/dsp/ToneShaper.cpp
    src/dsp/TruePeakLimiter.cpp
    src/dsp/Leveler.cpp
    src/dsp/NoiseSuppressor.cpp
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
        src/dsp/ChannelRepair.cpp
        src/dsp/PeakCompressor.cpp
        src/dsp/ToneShaper.cpp
        src/dsp/TruePeakLimiter.cpp
        src/dsp/Leveler.cpp
        src/dsp/NoiseSuppressor.cpp
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

    catch_discover_tests(SyncTrackPrepStateTests)
endif()
