cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/RubberBand.cmake")

add_executable(SliceLabRubberBandAllocations ../../tests/rubberband_allocations.cpp)
target_compile_features(SliceLabRubberBandAllocations PRIVATE cxx_std_20)
target_compile_options(SliceLabRubberBandAllocations PRIVATE -fno-builtin-malloc -fno-builtin-free)
target_link_libraries(SliceLabRubberBandAllocations PRIVATE SliceLabRubberBand)

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
    Source/MarkerImport.cpp
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
        SliceLabRubberBand
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
        juce::juce_audio_utils juce::juce_dsp juce::juce_cryptography SliceLabRubberBand)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/scheduled_engine_tests.cpp")
    add_executable(SliceLabScheduledEngineTests ../../tests/scheduled_engine_tests.cpp)
    target_compile_features(SliceLabScheduledEngineTests PRIVATE cxx_std_20)
    target_compile_definitions(SliceLabScheduledEngineTests PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1
        SLICELAB_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../..")
    target_include_directories(SliceLabScheduledEngineTests PRIVATE Source
        "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/audio-plugin-coder/_tools/JUCE/modules")
    target_link_libraries(SliceLabScheduledEngineTests PRIVATE SliceLab SliceLabRubberBand)
endif()

add_executable(SliceLabProcessorAllocations ../../tests/processor_allocations.cpp)
target_compile_features(SliceLabProcessorAllocations PRIVATE cxx_std_20)
target_compile_definitions(SliceLabProcessorAllocations PRIVATE
    JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1)
target_compile_options(SliceLabProcessorAllocations PRIVATE -fno-builtin-malloc -fno-builtin-free)
target_include_directories(SliceLabProcessorAllocations PRIVATE Source
    "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/audio-plugin-coder/_tools/JUCE/modules")
target_link_libraries(SliceLabProcessorAllocations PRIVATE SliceLab SliceLabRubberBand)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/marker_metronome_tests.cpp")
    add_executable(SliceLabMarkerMetronomeTests ../../tests/marker_metronome_tests.cpp)
    target_compile_features(SliceLabMarkerMetronomeTests PRIVATE cxx_std_20)
    target_compile_definitions(SliceLabMarkerMetronomeTests PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1)
    target_include_directories(SliceLabMarkerMetronomeTests PRIVATE Source
        "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/audio-plugin-coder/_tools/JUCE/modules")
    target_link_libraries(SliceLabMarkerMetronomeTests PRIVATE SliceLab SliceLabRubberBand)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/verify-separation-integration.cpp")
    add_executable(SliceLabSeparationIntegrationTests ../../tests/verify-separation-integration.cpp)
    target_compile_features(SliceLabSeparationIntegrationTests PRIVATE cxx_std_20)
    target_compile_definitions(SliceLabSeparationIntegrationTests PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1)
    target_include_directories(SliceLabSeparationIntegrationTests PRIVATE Source
        "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/audio-plugin-coder/_tools/JUCE/modules")
    target_link_libraries(SliceLabSeparationIntegrationTests PRIVATE SliceLab SliceLabRubberBand)
endif()

foreach(sanitizer IN ITEMS Address Thread)
    add_executable(SliceLabConcurrency${sanitizer}Tests EXCLUDE_FROM_ALL
        ../../tests/concurrency_stress.cpp Source/SamplerEngine.cpp)
    target_compile_features(SliceLabConcurrency${sanitizer}Tests PRIVATE cxx_std_20)
    target_compile_definitions(SliceLabConcurrency${sanitizer}Tests PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=1)
    target_include_directories(SliceLabConcurrency${sanitizer}Tests PRIVATE Source
        "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/audio-plugin-coder/_tools/JUCE/modules")
    target_link_libraries(SliceLabConcurrency${sanitizer}Tests PRIVATE SliceLab SliceLabRubberBand)
    if(sanitizer STREQUAL "Address")
        set(sanitizer_flags -fsanitize=address,undefined)
    else()
        set(sanitizer_flags -fsanitize=thread)
        # AbstractFifo's atomic handoff lives out of line in juce_core.cpp.
        # Instrument that entire translation unit so TSan can observe the
        # producer/consumer synchronization as well as the payload accesses.
        # The local object is linked before SliceLab's archive, preventing the
        # archive's uninstrumented juce_core object from being selected.
        target_sources(SliceLabConcurrency${sanitizer}Tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/../../vendor/audio-plugin-coder/_tools/JUCE/modules/juce_core/juce_core.cpp")
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_link_options(SliceLabConcurrency${sanitizer}Tests PRIVATE
                "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/SliceLabConcurrency${sanitizer}Tests.map")
        endif()
    endif()
    target_compile_options(SliceLabConcurrency${sanitizer}Tests PRIVATE ${sanitizer_flags} -g -fno-omit-frame-pointer)
    target_link_options(SliceLabConcurrency${sanitizer}Tests PRIVATE ${sanitizer_flags})
endforeach()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/processor_feature_tests.cpp")
    juce_add_console_app(SliceLabProcessorFeatureTests PRODUCT_NAME "SliceLab processor feature tests")
    target_sources(SliceLabProcessorFeatureTests PRIVATE ../../tests/processor_feature_tests.cpp)
    target_compile_features(SliceLabProcessorFeatureTests PRIVATE cxx_std_20)
    target_compile_definitions(SliceLabProcessorFeatureTests PRIVATE
        JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_VST3_CAN_REPLACE_VST2=0
        SLICELAB_TEST_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
    )
    target_include_directories(SliceLabProcessorFeatureTests PRIVATE Source)
    target_link_libraries(SliceLabProcessorFeatureTests PRIVATE SliceLab
        juce::juce_audio_utils juce::juce_dsp juce::juce_cryptography SliceLabRubberBand)
endif()
