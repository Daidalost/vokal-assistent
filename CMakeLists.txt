cmake_minimum_required(VERSION 3.22)
project(VokalAssistent VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static MSVC runtime: the plugin works without installing the Visual C++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(VA_BUILD_PLUGIN "Build the VST3 plugin (needs JUCE)" ON)
option(VA_BUILD_DEV_TOOLS "Build developer tools (UI snapshot)" OFF)

# ---------------------------------------------------------------- core (no JUCE)
add_library(va_core STATIC src/core/Analyzer.cpp)
target_include_directories(va_core PUBLIC src/core)
set_target_properties(va_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(va_core PRIVATE /W4 /wd4244 /wd4267)
else()
  target_compile_options(va_core PRIVATE -Wall -Wextra)
endif()

add_executable(va_tests tests/tests.cpp)
target_link_libraries(va_tests PRIVATE va_core)
add_executable(va_analyze tools/va_analyze.cpp)
target_link_libraries(va_analyze PRIVATE va_core)

enable_testing()
add_test(NAME core_tests COMMAND va_tests)

# ---------------------------------------------------------------- plugin
if(VA_BUILD_PLUGIN)
  include(FetchContent)
  # Set -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE to use a local copy.
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.4
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(JUCE)

  juce_add_plugin(VokalAssistent
    COMPANY_NAME "Daidalost"
    PLUGIN_MANUFACTURER_CODE Ddls
    PLUGIN_CODE VoAs
    FORMATS VST3
    PRODUCT_NAME "Vokal Assistent"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx EQ
    COPY_PLUGIN_AFTER_BUILD FALSE)

  target_sources(VokalAssistent PRIVATE
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/EqDisplay.cpp)

  target_compile_definitions(VokalAssistent PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

  target_link_libraries(VokalAssistent
    PRIVATE va_core juce::juce_audio_utils juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

  if(VA_BUILD_DEV_TOOLS)
    juce_add_console_app(va_ui_snapshot PRODUCT_NAME "va_ui_snapshot")
    target_sources(va_ui_snapshot PRIVATE tools/ui_snapshot.cpp)
    target_compile_definitions(va_ui_snapshot PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(va_ui_snapshot PRIVATE VokalAssistent va_core juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
  endif()
endif()
