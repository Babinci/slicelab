#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace slicelab {

constexpr int padCount = 32;
constexpr int firstMidiNote = 36;
constexpr int stemCount = 4;

enum class SliceMode { Transients, Grid, Equal, Random, FindBest };

struct PadSettings {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    float gainDb = 0.0f;
    float pan = 0.0f;
    float semitones = 0.0f;
    float cents = 0.0f;
    float timeRatio = 1.0f;
    float cutoffHz = 20000.0f;
    float resonance = 0.7071f;
    float attackMs = 2.0f;
    float releaseMs = 25.0f;
    bool reverse = false;
    bool loop = false;
    bool favorite = false;
    int output = 0;
    juce::String name;
    bool active() const { return endSeconds > startSeconds; }
};

struct GlobalSettings {
    double sourceBpm = 120.0;
    double targetBpm = 120.0;
    double beatOffsetSeconds = 0.0;
    float gainDb = -6.0f;
    float semitones = 0.0f;
    float cents = 0.0f;
    float hostMultiplier = 1.0f;
    bool sync = true;
    bool hold = false;
    bool mono = true;
    bool keyboardMode = false;
    bool repitch = false;
    bool velocitySensitive = true;
    bool snap = true;
    float glideMs = 0.0f;
    int keyOverride = -1;
    int rootMidiNote = 60;
    std::array<bool, stemCount> stemEnabled { true, true, true, true };
    std::array<float, stemCount> stemGainDb { 0.0f, 0.0f, 0.0f, 0.0f };
    std::array<bool, stemCount> stemSolo { false, false, false, false };
};

struct BeatAnchor {
    double sourceSeconds = 0.0;
    double beat = 0.0;
};

struct ViewState {
    int width = 1240;
    int height = 820;
    double viewStartSeconds = 0.0;
    double viewEndSeconds = 0.0;
    std::uint32_t selectionMask = 1;
    bool followPlayhead = false;
};

struct SessionState {
    int schema = 1;
    std::uint64_t revision = 0;
    std::uint64_t assetGeneration = 0;
    juce::String sourcePath;
    juce::String sourceHash;
    std::array<PadSettings, padCount> pads;
    GlobalSettings global;
    std::vector<BeatAnchor> beatAnchors;
    ViewState view;
    std::uint32_t randomSeed = 0x534c4142;
};

struct WavePeak { float minimum = 0.0f; float maximum = 0.0f; };

struct SampleAsset {
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    juce::String sourcePath;
    juce::String sourceHash;
    std::uint64_t generation = 0;
    std::vector<WavePeak> peaks;
    std::vector<double> transients;
    double detectedBpm = 0.0;
    float bpmConfidence = 0.0f;
    juce::String detectedKey = "Unknown";
    juce::String camelot;
    float keyConfidence = 0.0f;
    bool stemsReady = false;
    juce::String stemModel;
    juce::String stemManifestPath;
    std::array<juce::AudioBuffer<float>, stemCount> stems;
    std::array<juce::String, stemCount> stemPaths;
    double durationSeconds() const { return audio.getNumSamples() / sampleRate; }
};

struct Snapshot {
    SessionState session;
    std::shared_ptr<const SampleAsset> asset;
    int selectedPad = 0;
    double playheadSeconds = 0.0;
    double hostBpm = 120.0;
    std::array<float, padCount> activity {};
    float outputPeak = 0.0f;
    bool busy = false;
    float progress = -1.0f;
    juce::String status = "Drop a sample to begin";
    juce::String error;
    bool canUndo = false;
    bool canRedo = false;
};

}
