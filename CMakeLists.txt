cmake_minimum_required(VERSION 3.22)
project(WinVolumeFollow VERSION 0.1.0)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(WinVolumeFollow
    COMPANY_NAME "Personal"
    PLUGIN_MANUFACTURER_CODE Pers
    PLUGIN_CODE Wvfl
    FORMATS VST3
    PRODUCT_NAME "Windows Volume Follow"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(WinVolumeFollow PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/WindowsVolumeFollower.h)

target_compile_definitions(WinVolumeFollow PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(WinVolumeFollow PRIVATE
    juce::juce_audio_utils
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    ole32 uuid)
