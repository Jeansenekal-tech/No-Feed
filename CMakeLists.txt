cmake_minimum_required(VERSION 3.22)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NOFEED_VERSION LIMIT_COUNT 1)
project(NoFeed VERSION ${NOFEED_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")

# Link the C++ runtime statically on Windows so the plugin doesn't depend on the VC++
# redistributable being installed. Hosts blacklist plugins whose DLL dependencies are missing.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(NOFEED_BUILD_TESTS "Build the offline feedback-loop test" ON)

# JUCE is fetched automatically. To use a local checkout instead, configure with
#   -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

set(NOFEED_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND NOFEED_FORMATS AU)
endif()

juce_add_plugin(NoFeed
    COMPANY_NAME "UniqueAV"
    BUNDLE_ID "com.uniqueav.nofeed"
    PLUGIN_MANUFACTURER_CODE Uqav
    PLUGIN_CODE Nofd
    FORMATS ${NOFEED_FORMATS}
    PRODUCT_NAME "NoFeed"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Restoration
    AU_MAIN_TYPE kAudioUnitType_Effect
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(NoFeed PRIVATE
    Source/FeedbackEngine.cpp
    Source/DeEsser.cpp
    Source/AiVoiceEngine.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(NoFeed PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

# the trained network, embedded in the plugin binary
juce_add_binary_data(NoFeedModel SOURCES Resources/model.bin)

target_link_libraries(NoFeed
    PRIVATE
        NoFeedModel
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(NOFEED_BUILD_TESTS)
    juce_add_console_app(NoFeedTests PRODUCT_NAME "NoFeedTests")
    target_sources(NoFeedTests PRIVATE
        Tests/LoopTest.cpp
        Source/FeedbackEngine.cpp
        Source/DeEsser.cpp)
    target_include_directories(NoFeedTests PRIVATE Source)
    target_compile_definitions(NoFeedTests PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_link_libraries(NoFeedTests
        PRIVATE
            juce::juce_dsp
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()

if(NOFEED_BUILD_TESTS)
    juce_add_console_app(NoFeedAiTest PRODUCT_NAME "NoFeedAiTest")
    target_sources(NoFeedAiTest PRIVATE Tests/AiTest.cpp Source/AiVoiceEngine.cpp Source/FeedbackEngine.cpp)
    target_include_directories(NoFeedAiTest PRIVATE Source)
    target_compile_definitions(NoFeedAiTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(NoFeedAiTest
        PRIVATE juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
endif()
