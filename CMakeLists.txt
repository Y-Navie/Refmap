cmake_minimum_required(VERSION 3.22)

project(Refmap VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
endif()

if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

set(REFMAP_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND REFMAP_FORMATS AU)
endif()

juce_add_plugin(Refmap
    COMPANY_NAME "Yaniv"
    PLUGIN_MANUFACTURER_CODE Yanv
    PLUGIN_CODE Rfm1
    FORMATS ${REFMAP_FORMATS}
    PRODUCT_NAME "Refmap"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    VST3_CATEGORIES Fx Analyzer EQ
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(Refmap PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Refmap PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(Refmap
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Optional: offline test that runs the plugin on synthetic audio (cmake -DREFMAP_TESTS=ON)
option(REFMAP_TESTS "Build the offline test harness" OFF)
if(REFMAP_TESTS)
    juce_add_console_app(RefmapTest PRODUCT_NAME "RefmapTest")
    target_sources(RefmapTest PRIVATE tests/RefmapTest.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_include_directories(RefmapTest PRIVATE Source)
    target_compile_definitions(RefmapTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(RefmapTest PRIVATE juce::juce_audio_utils juce::juce_audio_formats juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
