cmake_minimum_required (VERSION 3.22)

project (drumSeq VERSION 0.1.0 LANGUAGES CXX)

set (CMAKE_CXX_STANDARD 20)
set (CMAKE_CXX_STANDARD_REQUIRED ON)
set (CMAKE_CXX_EXTENSIONS ON)
set (CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set (CMAKE_OSX_DEPLOYMENT_TARGET "11.0")

if (NOT DEFINED JUCE_DIR)
    set (JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/juce" CACHE PATH "Path to the JUCE framework root")
endif ()

if (NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message (FATAL_ERROR
        "JUCE not found at '${JUCE_DIR}'. Clone it there, or pass -DJUCE_DIR=... during configuration.")
endif ()

add_subdirectory ("${JUCE_DIR}" "${CMAKE_BINARY_DIR}/juce")

juce_add_plugin (drumSeq
    PRODUCT_NAME             "drumSeq"
    FORMATS                  AU VST3 Standalone
    PLUGIN_MANUFACTURER_CODE DmSq
    PLUGIN_CODE              Dsqr
    VERSION_STRING           ${PROJECT_VERSION}
    DESCRIPTION              "16-lane polyrhythmic drum sequencer"
    COMPANY_NAME             "drumSeq"
    IS_SYNTH                 TRUE
    IS_MIDI_EFFECT           FALSE
    NEEDS_MIDI_INPUT         TRUE
    NEEDS_MIDI_OUTPUT        TRUE
    EDITOR_NEEDS_KEYBOARD_FOCUS TRUE
)

target_link_libraries (drumSeq PRIVATE
    juce::juce_audio_processors
    juce::juce_audio_plugin_client
)

target_compile_definitions (drumSeq PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)

target_compile_options (drumSeq PRIVATE -Wall -Wextra)

target_sources (drumSeq PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/DesignSystem/SequencerDesignSystem.cpp
)

target_sources (drumSeq PRIVATE
    Source/PluginProcessor.h
    Source/PluginEditor.h
    Source/DesignSystem/SequencerDesignSystem.h
)

if (NOT CMAKE_BUILD_TYPE)
    set (CMAKE_BUILD_TYPE Release)
endif ()

set_property (CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)