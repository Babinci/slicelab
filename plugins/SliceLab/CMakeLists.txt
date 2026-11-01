cmake_minimum_required(VERSION 3.22)

find_package(PkgConfig REQUIRED)
pkg_check_modules(RUBBERBAND REQUIRED IMPORTED_TARGET rubberband)

juce_add_plugin(SliceLab
    VERSION 0.1.0
    COMPANY_NAME "SliceLab"
    COMPANY_WEBSITE "https://github.com/Babinci/slicelab"
    BUNDLE_ID "org.slicelab.instrument"
    PLUGIN_MANUFACTURER_CODE Slab
    PLUGIN_CODE Sl01
    FORMATS VST3 Standalone
    PRODUCT_NAME "SliceLab"
    DESCRIPTION "Stem sampling instrument"
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES Instrument Sampler
    COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(SliceLab PRIVATE
    Source/PluginProcessor.cpp
    Source/SamplerEngine.cpp
    Source/PluginEditor.cpp
)
target_compile_features(SliceLab PUBLIC cxx_std_20)
target_compile_definitions(SliceLab PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_VST3_CAN_REPLACE_VST2=0
    SLICELAB_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
)
target_include_directories(SliceLab PUBLIC Source)
target_link_libraries(SliceLab
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_cryptography
        PkgConfig::RUBBERBAND
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/engine_tests.cpp")
    juce_add_console_app(SliceLabEngineTests PRODUCT_NAME "SliceLab engine behavior tests")
    target_sources(SliceLabEngineTests PRIVATE ../../tests/engine_tests.cpp)
    target_compile_features(SliceLabEngineTests PRIVATE cxx_std_20)
    target_compile_definitions(SliceLabEngineTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0
        SLICELAB_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
    )
    target_include_directories(SliceLabEngineTests PRIVATE Source)
    target_link_libraries(SliceLabEngineTests PRIVATE SliceLab
        juce::juce_audio_utils juce::juce_dsp juce::juce_cryptography PkgConfig::RUBBERBAND)
endif()
