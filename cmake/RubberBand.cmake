find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3 REQUIRED IMPORTED_TARGET fftw3)
pkg_check_modules(SAMPLERATE REQUIRED IMPORTED_TARGET samplerate)

set(rb_source "${CMAKE_CURRENT_LIST_DIR}/../vendor/rubberband")
set(rb_build "${CMAKE_BINARY_DIR}/slicelab-rubberband")
file(COPY "${rb_source}/src" "${rb_source}/rubberband" DESTINATION "${rb_build}"
    PATTERN "BinClassifier.h" EXCLUDE)
file(READ "${rb_source}/src/finer/BinClassifier.h" rb_classifier)
set(rb_old [=[        while (m_vfQueue.getReadSpace() > 0) {
            process_t *entry = m_vfQueue.readOne();
            deallocate(entry);
        }
@RESET_BLANK@
        for (int i = 0; i < m_parameters.horizontalFilterLag; ++i) {
            process_t *entry =
                allocate_and_zero<process_t>(m_parameters.binCount);
            m_vfQueue.write(&entry, 1);
        }
]=])
string(REPLACE "@RESET_BLANK@" "        " rb_old "${rb_old}")
set(rb_new [=[        const int queued = m_vfQueue.getReadSpace();
        for (int i = 0; i < queued; ++i) {
            process_t *entry = m_vfQueue.readOne();
            for (int bin = 0; bin < m_parameters.binCount; ++bin) entry[bin] = 0;
            m_vfQueue.write(&entry, 1);
        }
]=])
string(FIND "${rb_classifier}" "${rb_old}" rb_patch_position)
if(rb_patch_position EQUAL -1)
    message(FATAL_ERROR "Rubber Band reset patch does not match the pinned source")
endif()
string(REPLACE "${rb_old}" "${rb_new}" rb_classifier "${rb_classifier}")
file(GENERATE OUTPUT "${rb_build}/src/finer/BinClassifier.h" CONTENT "${rb_classifier}")

set(rb_files
    RubberBandStretcher.cpp rubberband-c.cpp
    faster/AudioCurveCalculator.cpp faster/CompoundAudioCurve.cpp
    faster/HighFrequencyAudioCurve.cpp faster/SilentAudioCurve.cpp
    faster/PercussiveAudioCurve.cpp faster/R2Stretcher.cpp
    faster/StretcherChannelData.cpp faster/StretcherProcess.cpp
    common/Allocators.cpp common/FFT.cpp common/Log.cpp common/Profiler.cpp
    common/Resampler.cpp common/StretchCalculator.cpp common/sysutils.cpp
    common/mathmisc.cpp common/Thread.cpp finer/R3Stretcher.cpp)
list(TRANSFORM rb_files PREPEND "${rb_build}/src/")
add_library(SliceLabRubberBand STATIC ${rb_files})
target_compile_features(SliceLabRubberBand PUBLIC cxx_std_11)
target_compile_definitions(SliceLabRubberBand PRIVATE
    HAVE_FFTW3 FFTW_DOUBLE_ONLY HAVE_LIBSAMPLERATE
    NO_THREAD_CHECKS USE_PTHREADS NO_TIMING HAVE_POSIX_MEMALIGN NDEBUG)
target_include_directories(SliceLabRubberBand PUBLIC "${rb_build}"
    PRIVATE "${rb_build}/src" "${rb_build}/rubberband")
target_link_libraries(SliceLabRubberBand PRIVATE PkgConfig::FFTW3 PkgConfig::SAMPLERATE)
set_target_properties(SliceLabRubberBand PROPERTIES
    POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
