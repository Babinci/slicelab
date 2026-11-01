#pragma once

#include "Domain.h"
#include <array>
#include <memory>

namespace slicelab {

struct PlaybackPad {
    float gainDb = 0, pan = 0, semitones = 0, cents = 0, timeRatio = 1;
    float cutoffHz = 20000, resonance = 0.7071f, attackMs = 2, releaseMs = 25;
    bool reverse = false, loop = false;
    int output = 0;
};

struct PlaybackParameters {
    std::array<PlaybackPad, padCount> pads;
    double sourceBpm = 120, targetBpm = 120, hostBpm = 0;
    float gainDb = -6, semitones = 0, cents = 0, hostMultiplier = 1;
    bool sync = true, hold = false, mono = true, keyboardMode = false;
    bool repitch = false, velocitySensitive = true, metronome = false;
    float glideMs = 0;
    int rootMidiNote = 60, selectedPad = 0;
    std::array<bool, stemCount> stemEnabled { true, true, true, true };
    std::array<bool, stemCount> stemSolo { false, false, false, false };
    std::array<float, stemCount> stemGainDb { 0, 0, 0, 0 };
};

class SamplerEngine {
public:
    SamplerEngine();
    ~SamplerEngine();
    void prepare(double sampleRate, int maxBlockSize);
    void release();
    void publish(std::shared_ptr<const SampleAsset>, const SessionState&);
    void collectRetired();
    void enqueueTrigger(int pad, float velocity, bool noteOn);
    void enqueueNote(int midiNote, float velocity, bool noteOn);
    void enqueueStop();
    void enqueuePreview(double sourceSeconds);
    void enqueueStopPreview();
    void process(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
                 const PlaybackParameters&, const std::array<int, padCount>& outputChannelOffsets);
    // A zero latency request selects the engine's rate-scaled default. Prepare and
    // release only while callbacks are quiescent; both realtime and offline
    // callers use the same sample-time delay.
    void prepareScheduled(double sampleRate, int maximumBlockSize, int latencyFrames = 0);
    void processScheduled(juce::AudioBuffer<float>&, const juce::MidiBuffer&,
                          const PlaybackParameters&, const std::array<int, padCount>& outputChannelOffsets,
                          bool nonRealtime) noexcept;
    int scheduledLatencySamples() const noexcept;
    std::uint64_t scheduledOverloads() const noexcept;
    double playheadSeconds() const noexcept;
    float activity(int pad) const noexcept;
    float outputPeak() const noexcept;
    bool previewPlaying() const noexcept;
    bool voicesActive() const noexcept;
    std::uint64_t commandOverflows() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
