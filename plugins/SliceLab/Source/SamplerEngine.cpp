#include "SamplerEngine.h"
#include <rubberband/RubberBandStretcher.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <vector>

namespace slicelab {
namespace {
constexpr int voiceCount = 32;
constexpr int workSize = 1024;
constexpr int scheduledQuantum = 128;
constexpr int scheduledMidiCapacity = 65536;
constexpr int scheduledCommandCapacity = 4096;
constexpr int scheduledMaxMidiPerRequest = 4096;
constexpr int scheduledChannels = 64;
constexpr int commandCount = 256;
constexpr double pi = 3.14159265358979323846;

float dbToGain(float db) { return std::pow(10.0f, db / 20.0f); }

struct RealtimeLogger final : RubberBand::RubberBandStretcher::Logger {
    void log(const char*) override {}
    void log(const char*, double) override {}
    void log(const char*, double, double) override {}
};
struct Boundary { double start = 0, end = 0; };
struct View {
    std::shared_ptr<const SampleAsset> asset;
    std::array<Boundary, padCount> boundaries;
    std::vector<BeatAnchor> anchors;
    double beatOffset = 0;
};
struct Command { int pad = -1; float velocity = 0; bool on = false; double seconds = 0; };

struct Voice {
    std::unique_ptr<RubberBand::RubberBandStretcher> stretcher;
    const SampleAsset* asset = nullptr;
    const View* view = nullptr;
    int pad = -1, note = -1, channel = 0;
    bool active = false, released = false, keyReleased = false, outstanding = false, stretched = false, finalSent = false, sourceEnded = false, preview = false;
    double start = 0, end = 0, sourcePosition = 0, logicalPosition = 0;
    double timeRatio = 1, pitchScale = 1;
    float velocity = 1, envelope = 0;
    std::array<float, stemCount> stemMix { 1, 1, 1, 1 };
    float originalMix = 1, keyboardOffset = 0;
    std::array<float, 2> z1 {}, z2 {};
    int startPadding = 0, outputDelay = 0;
    // Preview-only click state; no audio or beat state is shared with pads.
    const View* clickView = nullptr;
    double clickSourceBpm = 0, clickBeat = 0;
    float clickOsc = 0, clickPrevious = 0, clickCoefficient = 0, clickAmplitude = 0;
    int clickRemaining = 0, clickLength = 0;
    bool clickInitialized = false;
    std::uint64_t age = 0, noteSequence = 0;
    std::array<std::array<float, workSize>, 2> input {}, output {};

    void reset() { active = false; asset = nullptr; }
};
}

struct SamplerEngine::Impl {
    struct ScheduledMidi {
        int offset = 0, size = 0;
        std::array<unsigned char, 3> bytes {};
    };
    struct ScheduledRequest {
        std::uint64_t start = 0, midiBegin = 0, midiEnd = 0, commandBegin = 0, commandEnd = 0;
        int frames = 0, channels = 0;
        PlaybackParameters parameters;
        std::array<int, padCount> offsets {};
    };
    struct ScheduledMeter {
        std::uint64_t end = 0;
        double playhead = 0;
        std::array<float, padCount> activity {};
        bool preview = false, anyVoice = false;
    };
    struct ScheduledVoiceWork {
        enum class Phase { Prepare, Reset, Ratio, Pitch, Pump, Mix } phase = Phase::Prepare;
        int done = 0, block = 0, channelOffset = 0;
        double ratio = 1, pitch = 1, directSpeed = 1, positionStep = 1;
        float gains[2] {}, attack = 1, release = 1, b0 = 1, b1 = 0, a1 = 0, a2 = 0;
        std::array<float, stemCount> targets {};
        bool originalTarget = true, needsStretch = false, newStretch = false;
    };
    struct ScheduledWork {
        enum class Phase { Begin, Commands, Segment, Voices, Voice } phase = Phase::Begin;
        std::uint64_t midiCursor = 0, commandCursor = 0, steps = 0;
        int cursor = 0, segmentEnd = 0, voiceIndex = 0;
        ScheduledVoiceWork voice;
    };
    struct Schedule {
        bool ready = false;
        int maxBlock = 0, latency = 0, layoutChannels = -1;
        std::array<int, padCount> layoutOffsets {};
        std::size_t requestMask = 0, outputMask = 0;
        std::uint64_t requestRead = 0, requestWrite = 0, midiRead = 0, midiWrite = 0;
        std::uint64_t commandRead = 0, commandWrite = 0, meterRead = 0, meterWrite = 0;
        std::uint64_t streamFrame = 0, epochStart = 0, completedUntil = 0;
        double credits = 0, workPlayhead = 0;
        std::vector<ScheduledRequest> requests;
        std::vector<ScheduledMeter> meters;
        std::vector<float> output;
        std::unique_ptr<std::atomic<const View*>[]> requestHazards;
        std::size_t hazardCount = 0;
        std::atomic<const View*> callHazard { nullptr };
        std::array<ScheduledMidi, scheduledMidiCapacity> midi;
        std::array<Command, scheduledCommandCapacity> commands;
        ScheduledWork work;
    } scheduled;
    std::atomic<std::uint64_t> scheduledFaults { 0 };
    bool deferTelemetry = false;
    double rate = 44100;
    std::shared_ptr<RubberBand::RubberBandStretcher::Logger> logger = std::make_shared<RealtimeLogger>();
    std::array<Voice, voiceCount> voices;
    std::array<std::atomic<const View*>, voiceCount> voiceViews {};
    std::shared_ptr<const View> current;
    std::vector<std::shared_ptr<const View>> retired;
    std::mutex viewsMutex;
    std::atomic<const View*> published { nullptr };
    std::atomic<const View*> retained { nullptr };
    std::atomic<const View*> acquiring { nullptr };
    const View* audioView = nullptr;
    juce::AbstractFifo commandsFifo { commandCount };
    std::array<Command, commandCount> commands;
    std::atomic<bool> commandOverflow { false };
    std::atomic<std::uint64_t> overflowCount { 0 };
    std::atomic<double> playhead { 0 };
    std::array<std::atomic<float>, padCount> activity {};
    std::atomic<float> peak { 0 };
    std::atomic<bool> previewIsPlaying { false }, anyVoiceActive { false };
    std::uint64_t age = 0;
    std::array<bool, 16> sustain {};
    std::array<std::array<std::uint64_t, 128>, 16> noteOns {}, noteOffs {};
    float stemSmoothing = 0.001f;

    void resetVoice(Voice& voice) {
        voice.outstanding=false;
        voice.reset();
        voiceViews[(size_t)(&voice - voices.data())].store(nullptr);
    }
    void clearNotes() { for(auto& channel:noteOns) channel.fill(0); for(auto& channel:noteOffs) channel.fill(0); sustain.fill(false); }

    void updateView() {
        const View* next = nullptr;
        bool acquired = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
            next = published.load();
            acquiring.store(next);
            if (next == published.load()) { acquired = true; break; }
        }
        if (!acquired) { acquiring.store(nullptr); return; }
        if (next != audioView) {
            retained.store(next);
            audioView = next;
        }
        acquiring.store(nullptr);
    }

    void enqueue(Command command) {
        const auto scope = commandsFifo.write(1);
        if (scope.blockSize1 > 0) commands[(size_t) scope.startIndex1] = command;
        else { commandOverflow.store(true, std::memory_order_relaxed); overflowCount.fetch_add(1, std::memory_order_relaxed); }
    }

    bool start(int pad, int note, int channel, float velocity, const PlaybackParameters& p, bool preview = false, double seconds = 0, std::uint64_t sequence = 0) {
        if (!audioView || !audioView->asset || pad < 0 || pad >= padCount) return false;
        const auto boundary = preview ? Boundary { std::max(0.0,seconds), audioView->asset->durationSeconds() } : audioView->boundaries[(size_t) pad];
        const auto* asset = audioView->asset.get();
        if (!std::isfinite(boundary.start) || !std::isfinite(boundary.end) || boundary.end <= boundary.start) return false;
        const double first = std::clamp(std::round(boundary.start * asset->sampleRate), 0.0, (double) asset->audio.getNumSamples());
        double last = std::clamp(std::round(boundary.end * asset->sampleRate), 0.0, (double) asset->audio.getNumSamples());
        if (last <= first && first < asset->audio.getNumSamples()) last = first + 1.0;
        if (last - first < 1.0) return false;
        if (preview) for (auto& voice : voices) if (voice.preview) resetVoice(voice);
        if (!preview && p.mono && p.keyboardMode && p.glideMs > 0) {
            for (auto& voice : voices) if (voice.active && !voice.preview && voice.pad == pad && !voice.released) {
                voice.note = note; voice.channel = channel; voice.noteSequence = sequence; voice.keyReleased = false; voice.outstanding = note >= 0 && channel >= 1 && channel <= 16;
                voice.velocity = p.velocitySensitive ? std::clamp(velocity, 0.0f, 1.0f) : 1.0f;
                return true;
            }
        }
        if (!preview && p.mono) for (auto& voice : voices) if (!voice.preview) resetVoice(voice);
        Voice* chosen = &voices[0];
        for (auto& voice : voices) {
            if (!voice.active) { chosen = &voice; break; }
            if (voice.age < chosen->age) chosen = &voice;
        }
        auto& v = *chosen;
        resetVoice(v);
        v.view = audioView; voiceViews[(size_t)(&v - voices.data())].store(audioView);
        v.asset = asset; v.pad = pad; v.note = note; v.channel = channel; v.preview = preview; v.velocity = p.velocitySensitive ? std::clamp(velocity, 0.0f, 1.0f) : 1.0f;
        v.keyboardOffset = p.keyboardMode && note >= 0 ? (float)(note - p.rootMidiNote) : 0;
        v.start = first; v.end = last; v.sourcePosition = !preview && p.pads[(size_t) pad].reverse ? last - 1.0 : first;
        v.logicalPosition = v.sourcePosition; v.envelope = 0; v.active = true; v.released = false;
        v.clickInitialized = false; v.clickRemaining = 0; v.clickView = nullptr;
        v.keyReleased = false; v.noteSequence = sequence; v.outstanding = note >= 0 && channel >= 1 && channel <= 16; v.stretched = false; v.finalSent = false; v.sourceEnded = false; v.startPadding = v.outputDelay = 0;
        v.z1 = {}; v.z2 = {}; v.age = ++age;
        const bool anySolo = std::any_of(p.stemSolo.begin(), p.stemSolo.end(), [](bool solo) { return solo; });
        bool original = true;
        for (int s = 0; s < stemCount; ++s) {
            const bool enabled = p.stemEnabled[(size_t)s] && (!anySolo || p.stemSolo[(size_t)s]);
            v.stemMix[(size_t) s] = enabled ? dbToGain(p.stemGainDb[(size_t) s]) : 0.0f;
            original = original && enabled && std::abs(p.stemGainDb[(size_t)s]) < 0.00001f;
        }
        v.originalMix = original ? 1.0f : 0.0f;
        if (!preview && !deferTelemetry) activity[(size_t) pad].store(v.velocity, std::memory_order_relaxed);
        return true;
    }

    void stop(int pad, int note, int channel, const PlaybackParameters& p) {
        std::uint64_t sequence=0;
        if (note >= 0 && channel >= 1 && channel <= 16) {
            auto& off=noteOffs[(size_t)(channel-1)][(size_t)note];
            const auto on=noteOns[(size_t)(channel-1)][(size_t)note];
            if(off>=on) return;
            sequence=++off;
        }
        if (note < 0 && !p.hold) return;
        Voice* oldest = nullptr;
        for (auto& voice : voices) {
            if (!voice.active || voice.keyReleased || voice.preview || (note >= 0 && !voice.outstanding)) continue;
            if (note >= 0 && (voice.note != note || voice.channel != channel || voice.noteSequence != sequence)) continue;
            if (note < 0 && (voice.pad != pad || voice.channel != 0 || voice.note != -1)) continue;
            if (!oldest || voice.age < oldest->age) oldest = &voice;
        }
        if (oldest) {
            oldest->keyReleased = true; oldest->outstanding=false;
            if (p.hold && (channel < 1 || !sustain[(size_t)(channel - 1)])) oldest->released = true;
        }
    }

    void event(const juce::MidiMessage& message, const PlaybackParameters& p) {
        const int note = message.getNoteNumber(), channel = message.getChannel();
        const int pad = p.keyboardMode ? p.selectedPad : note - firstMidiNote;
        if (message.isNoteOn() && channel >= 1 && channel <= 16) {
            const auto sequence=++noteOns[(size_t)(channel-1)][(size_t)note];
            start(pad, note, channel, message.getFloatVelocity(), p, false, 0, sequence);
        } else if (message.isNoteOff()) stop(pad, note, channel, p);
        else if (message.isAllNotesOff() || message.isAllSoundOff()) {
            for (auto& voice : voices) if (voice.channel == channel) resetVoice(voice);
            if (channel >= 1 && channel <= 16) { sustain[(size_t)(channel-1)] = false; noteOns[(size_t)(channel-1)].fill(0); noteOffs[(size_t)(channel-1)].fill(0); }
        } else if (message.isController() && message.getControllerNumber() == 64 && channel >= 1 && channel <= 16) {
            sustain[(size_t)(channel-1)] = message.getControllerValue() >= 64;
            if (p.hold && !sustain[(size_t)(channel-1)]) for (auto& voice : voices)
                if (voice.channel == channel && voice.keyReleased) voice.released = true;
        }
    }

    float source(Voice& v, int channel, double position, const std::array<float, stemCount>& targets, bool originalTarget, bool loop) {
        const auto& a = *v.asset;
        const int n = a.audio.getNumSamples();
        const int first=std::clamp((int)v.start,0,n-1), last=std::clamp((int)v.end-1,first,n-1);
        // A reverse slice holds its first frame over the final fractional
        // frame interval. A loop instead interpolates across the seam.
        if (position < v.start) {
            if (loop) {
                const double length = v.end - v.start;
                double wrapped = std::fmod(position - v.start, length);
                if (wrapped < 0) wrapped += length;
                position = v.start + wrapped;
            } else position = v.start;
        }
        const int i0 = std::clamp((int) position, first, last);
        const int i1 = i0 < last ? i0 + 1 : (loop ? first : last);
        const float frac = (float) (position - std::floor(position));
        auto interpolate = [=](const juce::AudioBuffer<float>& b) {
            const float* data = b.getReadPointer(std::min(channel, b.getNumChannels() - 1));
            return data[i0] + frac * (data[i1] - data[i0]);
        };
        if (channel == 0) {
            for (int s = 0; s < stemCount; ++s) v.stemMix[(size_t)s] += (targets[(size_t)s] - v.stemMix[(size_t)s]) * stemSmoothing;
            v.originalMix += ((originalTarget ? 1.0f : 0.0f) - v.originalMix) * stemSmoothing;
        }
        if (!a.stemsReady) return originalTarget ? interpolate(a.audio) : 0.0f;
        if (v.originalMix > 0.99999f && originalTarget) return interpolate(a.audio);
        float separated = 0;
        for (int s = 0; s < stemCount; ++s) separated += v.stemMix[(size_t)s] * interpolate(a.stems[(size_t)s]);
        return interpolate(a.audio) * v.originalMix + separated * (1.0f - v.originalMix);
    }

    bool advance(Voice& v, double& position, double amount, bool reverse, bool loop) {
        position += reverse ? -amount : amount;
        // Reverse begins on end - 1, so its half-open coordinate interval is
        // (start - 1, end - 1]. This gives the same duration as [start, end)
        // in forward playback, including fractional source-frame steps.
        if (reverse ? position > v.start - 1.0 : position < v.end) return true;
        if (!loop) return false;
        const double length = v.end - v.start;
        if (!(length >= 1.0) || !std::isfinite(position)) return false;
        double wrapped = std::fmod(position - v.start, length);
        if (wrapped < 0) wrapped += length;
        position = v.start + wrapped;
        return true;
    }

    // The logical position describes samples emerging from the voice, whereas
    // sourcePosition may be far ahead while Rubber Band is being primed. This
    // keeps the click aligned with audible preview samples in both renderers.
    float previewClick(Voice& v, const PlaybackParameters& p) noexcept {
        const auto* view = audioView && audioView->asset.get() == v.asset ? audioView : v.view;
        if (!view) return 0.0f;
        const double seconds = v.logicalPosition / v.asset->sampleRate - view->beatOffset;
        double beat = seconds * p.sourceBpm / 60.0;
        if (view->anchors.size() >= 2) {
            const auto& anchors = view->anchors;
            auto upper = std::upper_bound(anchors.begin(), anchors.end(), seconds,
                [](double t, const BeatAnchor& anchor) { return t < anchor.sourceSeconds; });
            if (upper == anchors.begin()) ++upper;
            if (upper == anchors.end()) --upper;
            const auto& a = *(upper - 1); const auto& b = *upper;
            beat = a.beat + (seconds - a.sourceSeconds) * (b.beat - a.beat) / (b.sourceSeconds - a.sourceSeconds);
        }
        if (!std::isfinite(beat)) return 0.0f;
        // Tolerate floating-point noise at an exact grid line without moving
        // a click by an audible fraction of an output sample.
        constexpr double epsilon = 1.0e-9;
        const double index = std::floor(beat + epsilon);
        const bool bpmChanged = v.clickSourceBpm < p.sourceBpm || v.clickSourceBpm > p.sourceBpm;
        const bool reset = !v.clickInitialized || v.clickView != view || bpmChanged || beat < v.clickBeat - epsilon;
        const bool crossed = reset ? std::abs(beat - std::round(beat)) <= epsilon
                                   : index > std::floor(v.clickBeat + epsilon);
        v.clickView = view; v.clickSourceBpm = p.sourceBpm;
        v.clickBeat = beat; v.clickInitialized = true;
        if (crossed) {
            const bool accent = std::abs(std::fmod(index, 4.0)) < epsilon;
            const double frequency = accent ? 1500.0 : 1000.0;
            const float cosine = (float)std::cos(2.0 * pi * frequency / rate);
            v.clickCoefficient = 2.0f * cosine;
            v.clickOsc = 1.0f; v.clickPrevious = cosine;
            v.clickAmplitude = accent ? 0.08f : 0.05f;
            v.clickLength = std::max(1, (int)std::round(rate * 0.008));
            v.clickRemaining = v.clickLength;
        }
        if (v.clickRemaining <= 0) return 0.0f;
        const float output = v.clickAmplitude * v.clickOsc * ((float)v.clickRemaining / (float)v.clickLength);
        const float next = v.clickCoefficient * v.clickOsc - v.clickPrevious;
        v.clickPrevious = v.clickOsc; v.clickOsc = next; --v.clickRemaining;
        return output;
    }

    void configureStretch(Voice& v, double ratio, double pitch) {
        auto& r = *v.stretcher;
        r.reset(); r.setTimeRatio(ratio); r.setPitchScale(pitch);
        v.timeRatio = ratio; v.pitchScale = pitch; v.stretched = true; v.finalSent = false; v.sourceEnded = false;
        v.sourcePosition = v.logicalPosition;
        v.startPadding = (int) r.getPreferredStartPad();
        v.outputDelay = (int) r.getStartDelay();
    }

    bool fill(Voice& v, const PlaybackParameters& p, const std::array<float, stemCount>& targets, bool originalTarget,
              int maxCount = workSize, bool boundedTail = false) {
        auto& r = *v.stretcher;
        if (v.finalSent) return false;
        int count = (int) std::clamp<std::size_t>(r.getSamplesRequired(), 1, (std::size_t)maxCount);
        bool final = false;
        const PlaybackPad previewPad {};
        const auto& pad = v.preview ? previewPad : p.pads[(size_t) v.pad];
        if (boundedTail && v.sourceEnded && pad.loop) {
            const double length = v.end - v.start;
            double wrapped = std::fmod(v.sourcePosition - v.start, length);
            if (wrapped < 0) wrapped += length;
            v.sourcePosition = v.start + wrapped;
            v.sourceEnded = false;
        }
        if (v.startPadding > 0 || (boundedTail && v.sourceEnded)) {
            if (v.startPadding > 0) {
            count = std::min(count, v.startPadding);
                v.startPadding -= count;
            }
            for (auto& channel : v.input) std::fill_n(channel.data(), count, 0.0f);
        } else {
            const double step = v.asset->sampleRate / rate;
            int written = 0;
            for (; written < count; ++written) {
                for (int channel = 0; channel < 2; ++channel)
                    v.input[(size_t) channel][(size_t) written] = source(v, channel, v.sourcePosition, targets, originalTarget, pad.loop);
                if (!advance(v, v.sourcePosition, step, pad.reverse, pad.loop)) { ++written; final = true; v.sourceEnded = true; break; }
            }
            count = written;
        }
        const float* input[] = { v.input[0].data(), v.input[1].data() };
        // R3's final=true path drains many FFT windows in one call. The
        // scheduled renderer instead feeds bounded zero chunks and stops at
        // the logical source boundary, so one call cannot drain the tail.
        r.process(input, (std::size_t) count, final && !boundedTail);
        v.finalSent = final && !boundedTail;
        return true;
    }

    void renderVoice(Voice& v, juce::AudioBuffer<float>& out, int startSample, int count,
                     const PlaybackParameters& p, const std::array<int, padCount>& offsets, bool preferPreview) {
        if (!v.active || !v.asset || !v.stretcher) return;
        const PlaybackPad previewPad {};
        const auto& pad = v.preview ? previewPad : p.pads[(size_t) v.pad];
        const bool anySolo = std::any_of(p.stemSolo.begin(), p.stemSolo.end(), [](bool solo) { return solo; });
        std::array<float, stemCount> targets {}; bool originalTarget = true;
        for (int s=0; s<stemCount; ++s) {
            const bool enabled = p.stemEnabled[(size_t)s] && (!anySolo || p.stemSolo[(size_t)s]);
            targets[(size_t)s] = enabled ? dbToGain(p.stemGainDb[(size_t)s]) : 0.0f;
            originalTarget = originalTarget && enabled && std::abs(p.stemGainDb[(size_t)s]) < 0.00001f;
        }
        const double tempo = p.sync && p.hostBpm > 1 ? p.hostBpm * p.hostMultiplier : p.targetBpm;
        double localBpm = p.sourceBpm;
        const auto* beatView = audioView && audioView->asset.get() == v.asset ? audioView : v.view;
        if (beatView && beatView->anchors.size() >= 2) {
            const double time = v.logicalPosition / v.asset->sampleRate - beatView->beatOffset;
            const auto& anchors = beatView->anchors;
            auto upper = std::upper_bound(anchors.begin(), anchors.end(), time, [](double t, const BeatAnchor& a) { return t < a.sourceSeconds; });
            if (upper == anchors.begin()) ++upper;
            if (upper == anchors.end()) --upper;
            const auto& first = *(upper - 1); const auto& second = *upper;
            localBpm = 60.0 * (second.beat - first.beat) / (second.sourceSeconds - first.sourceSeconds);
        }
        const double syncRatio = localBpm > 1 && tempo > 1 ? localBpm / tempo : 1.0;
        const double ratio = std::clamp((double) pad.timeRatio * syncRatio, 0.25, 4.0);
        const double keyboardTarget = !v.preview && p.keyboardMode && v.note >= 0 ? (double)(v.note - p.rootMidiNote) : 0.0;
        if (p.glideMs > 0) {
            const double fraction = std::min(1.0, count / (rate * p.glideMs * 0.001));
            v.keyboardOffset += (float)((keyboardTarget - v.keyboardOffset) * fraction);
        } else v.keyboardOffset = (float)keyboardTarget;
        const double pitch = std::clamp(std::pow(2.0, (p.semitones + pad.semitones + v.keyboardOffset + (p.cents + pad.cents) * 0.01) / 12.0), 0.25, 4.0);
        const double directSpeed = p.repitch ? pitch / ratio : 1.0;
        const bool needsStretch = !p.repitch && (std::abs(ratio - 1.0) > 0.00001 || std::abs(pitch - 1.0) > 0.00001);
        if (needsStretch && !v.stretched) configureStretch(v, ratio, pitch);
        if (!needsStretch && v.stretched) { v.stretched = false; v.sourcePosition = v.logicalPosition; }
        if (v.stretched) {
            if (std::abs(ratio - v.timeRatio) > 0.00001) { v.stretcher->setTimeRatio(ratio); v.timeRatio = ratio; }
            if (std::abs(pitch - v.pitchScale) > 0.00001) { v.stretcher->setPitchScale(pitch); v.pitchScale = pitch; }
        }
        const float master = dbToGain(p.gainDb + pad.gainDb) * v.velocity;
        const float angle = (std::clamp(pad.pan, -1.0f, 1.0f) + 1.0f) * (float) pi * 0.25f;
        const float gains[] = { master * std::cos(angle) * 1.41421356f, master * std::sin(angle) * 1.41421356f };
        const float attack = pad.attackMs <= 0 ? 1.0f : 1.0f / (float) (rate * pad.attackMs * 0.001);
        const float release = pad.releaseMs <= 0 ? 1.0f : 1.0f / (float) (rate * pad.releaseMs * 0.001);
        const double omega = 2.0 * pi * std::clamp((double) pad.cutoffHz, 20.0, rate * 0.45) / rate;
        const double alpha = std::sin(omega) / (2.0 * std::clamp((double) pad.resonance, 0.1, 10.0));
        const double norm = 1.0 / (1.0 + alpha), cosine = std::cos(omega);
        const float b0 = (float) ((1.0 - cosine) * 0.5 * norm), b1 = 2 * b0;
        const float a1 = (float) (-2.0 * cosine * norm), a2 = (float) ((1.0 - alpha) * norm);
        if (v.preview && !p.metronome) { v.clickInitialized = false; v.clickRemaining = 0; }
        int done = 0, work = 0;
        const int channelOffset = offsets[(size_t) std::clamp(pad.output, 0, padCount - 1)];
        while (done < count && v.active) {
            int block = std::min(workSize, count - done);
            if (v.stretched) {
                auto& r = *v.stretcher;
                while ((v.outputDelay > 0 || r.available() <= 0) && work < 128) {
                    const int available = std::max(0, r.available());
                    if (available > 0 && v.outputDelay > 0) {
                        const int drop = std::min({available, v.outputDelay, workSize});
                        float* output[] = { v.output[0].data(), v.output[1].data() };
                        v.outputDelay -= (int) r.retrieve(output, (std::size_t) drop);
                    } else if (!fill(v, p, targets, originalTarget)) break;
                    ++work;
                }
                if (v.outputDelay > 0 || r.available() <= 0) {
                    if (v.finalSent && r.available() <= 0) v.active = false;
                    break;
                }
                block = std::min(block, r.available());
                float* output[] = { v.output[0].data(), v.output[1].data() };
                block = (int) r.retrieve(output, (std::size_t) block);
                if (block <= 0) break;
            } else {
                block = 0;
                for (; block < std::min(workSize, count - done); ++block) {
                    for (int channel = 0; channel < 2; ++channel)
                        v.output[(size_t) channel][(size_t) block] = source(v, channel, v.sourcePosition, targets, originalTarget, pad.loop);
                    if (!advance(v, v.sourcePosition, v.asset->sampleRate / rate * directSpeed, pad.reverse, pad.loop)) { ++block; break; }
                }
            }
            for (int i = 0; i < block; ++i) {
                v.envelope = v.released ? std::max(0.0f, v.envelope - release) : std::min(1.0f, v.envelope + attack);
                if (v.released && v.envelope <= 0) { v.active = false; break; }
                const double positionStep = v.asset->sampleRate / rate * (p.repitch ? directSpeed : 1.0 / ratio);
                const double remaining = pad.reverse ? v.logicalPosition-v.start+1.0 : v.end-v.logicalPosition;
                const float boundaryFade = pad.loop ? 1.0f : (float)std::clamp(remaining / std::max(1.0,positionStep*rate*0.002),0.0,1.0);
                const bool mixClick = v.preview && p.metronome;
                const float click = mixClick ? previewClick(v, p) : 0.0f;
                for (int channel = 0; channel < 2; ++channel) {
                    const float x = v.output[(size_t) channel][(size_t) i];
                    const float y = v.preview ? x : b0 * x + v.z1[(size_t) channel];
                    if (!v.preview) {
                        v.z1[(size_t) channel] = b1 * x - a1 * y + v.z2[(size_t) channel];
                        v.z2[(size_t) channel] = b0 * x - a2 * y;
                    }
                    if (channelOffset + channel < out.getNumChannels()) {
                        const float dry = y * gains[channel] * v.envelope * boundaryFade;
                        out.addSample(channelOffset + channel, startSample + done + i, mixClick ? dry + click * gains[channel] : dry);
                    }
                }
                if (!advance(v, v.logicalPosition, positionStep, pad.reverse, pad.loop) && !pad.loop) {
                    v.active = false; break;
                }
            }
            done += block;
            if (!v.stretched && !pad.loop && (pad.reverse ? v.sourcePosition <= v.start - 1.0 : v.sourcePosition >= v.end)) v.active = false;
        }
        if (v.preview || !preferPreview) playhead.store(v.logicalPosition / v.asset->sampleRate, std::memory_order_relaxed);
        if (!v.preview) activity[(size_t) v.pad].store(v.active ? v.envelope * v.velocity : 0.0f, std::memory_order_relaxed);
        if (!v.active) resetVoice(v);
    }

    void render(juce::AudioBuffer<float>& out, int startSample, int count, const PlaybackParameters& p,
                const std::array<int, padCount>& offsets) {
        if (count <= 0) return;
        bool hasPreview = false;
        for (const auto& voice : voices) hasPreview = hasPreview || (voice.active && voice.preview);
        for (auto& voice : voices) renderVoice(voice, out, startSample, count, p, offsets, hasPreview);
    }

    void applyCommand(const Command& command, const PlaybackParameters& p) {
        if (command.pad == -1) { for (auto& voice : voices) resetVoice(voice); clearNotes(); }
        else if (command.pad == -2) start(0, -1, 0, 1.0f, p, true, command.seconds);
        else if (command.pad == -3) { for (auto& voice : voices) if (voice.preview) resetVoice(voice); }
        else if (command.pad == -4) {
            const int note = std::clamp((int)command.seconds, 0, 127);
            const auto message = command.on ? juce::MidiMessage::noteOn(1, note, command.velocity) : juce::MidiMessage::noteOff(1, note);
            event(message, p);
        } else if (command.on) start(command.pad, -1, 0, command.velocity, p);
        else stop(command.pad, -1, 0, p);
    }

    bool captureScheduledView() noexcept {
        auto& s = scheduled;
        for (int i = 0; i < 3; ++i) {
            const auto* view = published.load();
            s.callHazard.store(view);
            if (view == published.load()) return true;
        }
        s.callHazard.store(nullptr);
        return false;
    }

    // All rings below are owned by the serialized audio callback. Only the
    // view hazards cross threads, and their SC ordering permits a collector to
    // observe a handoff from call -> request -> retained -> voice.
    bool submitScheduled(const juce::MidiBuffer& midi, int hostFrames, int from, int count,
                         bool firstChunk, int channels, const PlaybackParameters& p,
                         const std::array<int, padCount>& offsets) noexcept {
        auto& s = scheduled;
        const auto capacity = s.requestMask + 1;
        if (s.requestWrite - s.requestRead >= capacity) return false;
        int midiCount = 0, inspected = 0;
        for (const auto metadata : midi) {
            if (++inspected > scheduledMaxMidiPerRequest) return false;
            const int pos = std::clamp(metadata.samplePosition, 0, hostFrames);
            if ((firstChunk ? pos >= from : pos > from) && pos <= from + count &&
                metadata.numBytes > 0 && metadata.numBytes <= 3 && metadata.data[0] < 0xf0) ++midiCount;
        }
        const int commandCountReady = firstChunk ? std::min(commandCount, commandsFifo.getNumReady()) : 0;
        if (midiCount > scheduledMaxMidiPerRequest || s.midiWrite - s.midiRead + (std::uint64_t)midiCount > scheduledMidiCapacity ||
            s.commandWrite - s.commandRead + (std::uint64_t)commandCountReady > scheduledCommandCapacity) return false;

        const auto slot = (std::size_t)(s.requestWrite & s.requestMask);
        auto& request = s.requests[slot];
        request.start = s.streamFrame; request.frames = count; request.channels = channels;
        request.parameters = p; request.offsets = offsets;
        request.midiBegin = s.midiWrite; request.commandBegin = s.commandWrite;
        for (const auto metadata : midi) {
            const int pos = std::clamp(metadata.samplePosition, 0, hostFrames);
            if ((firstChunk ? pos >= from : pos > from) && pos <= from + count &&
                metadata.numBytes > 0 && metadata.numBytes <= 3 && metadata.data[0] < 0xf0) {
                auto& item = s.midi[(std::size_t)(s.midiWrite++ % scheduledMidiCapacity)];
                item.offset = pos - from; item.size = metadata.numBytes;
                for (int n = 0; n < item.size; ++n) item.bytes[(std::size_t)n] = metadata.data[n];
            }
        }
        for (int i = 0; i < commandCountReady; ++i) {
            const auto scope = commandsFifo.read(1);
            if (scope.blockSize1 <= 0) return false;
            s.commands[(std::size_t)(s.commandWrite++ % scheduledCommandCapacity)] = commands[(std::size_t)scope.startIndex1];
        }
        request.midiEnd = s.midiWrite; request.commandEnd = s.commandWrite;
        s.requestHazards[slot].store(s.callHazard.load());
        const auto ringFrames = s.outputMask + 1;
        for (int channel = 0; channel < channels; ++channel) {
            auto* plane = s.output.data() + (std::size_t)channel * ringFrames;
            for (int i = 0; i < count; ++i) plane[(std::size_t)((s.streamFrame + (std::uint64_t)i) & s.outputMask)] = 0;
        }
        ++s.requestWrite;
        return true;
    }

    void failScheduled(std::uint64_t resumeFrame) noexcept {
        auto& s = scheduled;
        scheduledFaults.fetch_add(1, std::memory_order_relaxed);
        for (auto& voice : voices) resetVoice(voice);
        clearNotes();
        for (auto index = s.requestRead; index < s.requestWrite; ++index)
            s.requestHazards[(std::size_t)(index & s.requestMask)].store(nullptr);
        s.callHazard.store(nullptr);
        audioView = nullptr; retained.store(nullptr);
        s.requestRead = s.requestWrite; s.midiRead = s.midiWrite; s.commandRead = s.commandWrite;
        s.meterRead = s.meterWrite; s.work = {}; s.credits = 0;
        s.streamFrame = s.epochStart = s.completedUntil = resumeFrame;
        commandOverflow.store(false, std::memory_order_relaxed);
        const int queued = std::min(commandCount, commandsFifo.getNumReady());
        for (int i = 0; i < queued; ++i) { const auto scope = commandsFifo.read(1); (void)scope; }
        for (auto& a : activity) a.store(0, std::memory_order_relaxed);
        previewIsPlaying.store(false, std::memory_order_relaxed);
        anyVoiceActive.store(false, std::memory_order_relaxed);
        peak.store(0, std::memory_order_relaxed);
    }

    void finishScheduledVoice(Voice& v) noexcept {
        auto& s = scheduled;
        bool preferPreview = false;
        for (const auto& voice : voices) preferPreview = preferPreview || (voice.active && voice.preview);
        if (v.asset && (v.preview || !preferPreview)) s.workPlayhead = v.logicalPosition / v.asset->sampleRate;
        if (!v.active) resetVoice(v);
        ++s.work.voiceIndex;
        s.work.phase = ScheduledWork::Phase::Voices;
    }

    // One call does one bounded preparation, R3 operation, source generation,
    // retrieval, or mix. It returns true only when it called R3::process.
    bool scheduledVoiceStep(ScheduledRequest& request, Voice& v) noexcept {
        auto& s = scheduled;
        auto& job = s.work;
        auto& w = job.voice;
        const auto& p = request.parameters;
        const PlaybackPad previewPad {};
        const auto& pad = v.preview ? previewPad : p.pads[(std::size_t)v.pad];
        const int count = job.segmentEnd - job.cursor;
        using Phase = ScheduledVoiceWork::Phase;
        if (!v.active || !v.asset || !v.stretcher) { finishScheduledVoice(v); return false; }
        if (w.phase == Phase::Prepare) {
            if (v.preview && !p.metronome) { v.clickInitialized = false; v.clickRemaining = 0; }
            const bool anySolo = std::any_of(p.stemSolo.begin(), p.stemSolo.end(), [](bool solo) { return solo; });
            w.originalTarget = true;
            for (int i = 0; i < stemCount; ++i) {
                const bool enabled = p.stemEnabled[(std::size_t)i] && (!anySolo || p.stemSolo[(std::size_t)i]);
                w.targets[(std::size_t)i] = enabled ? dbToGain(p.stemGainDb[(std::size_t)i]) : 0.0f;
                w.originalTarget = w.originalTarget && enabled && std::abs(p.stemGainDb[(std::size_t)i]) < 0.00001f;
            }
            const double tempo = p.sync && p.hostBpm > 1 ? p.hostBpm * p.hostMultiplier : p.targetBpm;
            double localBpm = p.sourceBpm;
            const auto* beatView = audioView && audioView->asset.get() == v.asset ? audioView : v.view;
            if (beatView && beatView->anchors.size() >= 2) {
                const double time = v.logicalPosition / v.asset->sampleRate - beatView->beatOffset;
                const auto& anchors = beatView->anchors;
                auto upper = std::upper_bound(anchors.begin(), anchors.end(), time, [](double t, const BeatAnchor& a) { return t < a.sourceSeconds; });
                if (upper == anchors.begin()) ++upper;
                if (upper == anchors.end()) --upper;
                const auto& a = *(upper - 1); const auto& b = *upper;
                localBpm = 60.0 * (b.beat - a.beat) / (b.sourceSeconds - a.sourceSeconds);
            }
            const double syncRatio = localBpm > 1 && tempo > 1 ? localBpm / tempo : 1.0;
            w.ratio = std::clamp((double)pad.timeRatio * syncRatio, 0.25, 4.0);
            const double keyboardTarget = !v.preview && p.keyboardMode && v.note >= 0 ? (double)(v.note - p.rootMidiNote) : 0.0;
            if (p.glideMs > 0) {
                const double fraction = std::min(1.0, count / (rate * p.glideMs * 0.001));
                v.keyboardOffset += (float)((keyboardTarget - v.keyboardOffset) * fraction);
            } else v.keyboardOffset = (float)keyboardTarget;
            w.pitch = std::clamp(std::pow(2.0, (p.semitones + pad.semitones + v.keyboardOffset + (p.cents + pad.cents) * 0.01) / 12.0), 0.25, 4.0);
            w.directSpeed = p.repitch ? w.pitch / w.ratio : 1.0;
            w.positionStep = v.asset->sampleRate / rate * (p.repitch ? w.directSpeed : 1.0 / w.ratio);
            w.needsStretch = !p.repitch && (std::abs(w.ratio - 1.0) > 0.00001 || std::abs(w.pitch - 1.0) > 0.00001);
            w.newStretch = w.needsStretch && !v.stretched;
            if (!w.needsStretch && v.stretched) { v.stretched = false; v.sourcePosition = v.logicalPosition; }
            const float master = dbToGain(p.gainDb + pad.gainDb) * v.velocity;
            const float angle = (std::clamp(pad.pan, -1.0f, 1.0f) + 1.0f) * (float)pi * 0.25f;
            w.gains[0] = master * std::cos(angle) * 1.41421356f;
            w.gains[1] = master * std::sin(angle) * 1.41421356f;
            w.attack = pad.attackMs <= 0 ? 1.0f : 1.0f / (float)(rate * pad.attackMs * 0.001);
            w.release = pad.releaseMs <= 0 ? 1.0f : 1.0f / (float)(rate * pad.releaseMs * 0.001);
            const double omega = 2.0 * pi * std::clamp((double)pad.cutoffHz, 20.0, rate * 0.45) / rate;
            const double alpha = std::sin(omega) / (2.0 * std::clamp((double)pad.resonance, 0.1, 10.0));
            const double norm = 1.0 / (1.0 + alpha), cosine = std::cos(omega);
            w.b0 = (float)((1.0 - cosine) * 0.5 * norm); w.b1 = 2 * w.b0;
            w.a1 = (float)(-2.0 * cosine * norm); w.a2 = (float)((1.0 - alpha) * norm);
            w.channelOffset = request.offsets[(std::size_t)std::clamp(pad.output, 0, padCount - 1)];
            w.phase = w.newStretch ? Phase::Reset : (w.needsStretch ? Phase::Ratio : Phase::Pump);
            return false;
        }
        if (w.phase == Phase::Reset) {
            v.stretcher->reset(); v.stretched = true; v.finalSent = false; v.sourceEnded = false; v.sourcePosition = v.logicalPosition;
            w.phase = Phase::Ratio; return false;
        }
        if (w.phase == Phase::Ratio) {
            if (w.newStretch || std::abs(w.ratio - v.timeRatio) > 0.00001) {
                v.stretcher->setTimeRatio(w.ratio); v.timeRatio = w.ratio;
            }
            w.phase = Phase::Pitch; return false;
        }
        if (w.phase == Phase::Pitch) {
            if (w.newStretch || std::abs(w.pitch - v.pitchScale) > 0.00001) {
                v.stretcher->setPitchScale(w.pitch); v.pitchScale = w.pitch;
            }
            if (w.newStretch) {
                v.startPadding = (int)v.stretcher->getPreferredStartPad();
                v.outputDelay = (int)v.stretcher->getStartDelay();
            }
            w.phase = Phase::Pump; return false;
        }
        if (w.phase == Phase::Pump) {
            if (w.done >= count) { finishScheduledVoice(v); return false; }
            if (v.stretched) {
                auto& r = *v.stretcher;
                const int available = std::max(0, r.available());
                if (available > 0 && v.outputDelay > 0) {
                    float* out[] = { v.output[0].data(), v.output[1].data() };
                    v.outputDelay -= (int)r.retrieve(out, (std::size_t)std::min({available, v.outputDelay, scheduledQuantum}));
                    return false;
                }
                if (v.outputDelay > 0 || available <= 0) {
                    if (!fill(v, p, w.targets, w.originalTarget, scheduledQuantum, true)) {
                        v.active = false; finishScheduledVoice(v); return false;
                    }
                    return true;
                }
                float* out[] = { v.output[0].data(), v.output[1].data() };
                w.block = (int)r.retrieve(out, (std::size_t)std::min({available, count - w.done, scheduledQuantum}));
                if (w.block <= 0) { v.active = false; finishScheduledVoice(v); return false; }
            } else {
                w.block = 0;
                for (; w.block < std::min(scheduledQuantum, count - w.done); ++w.block) {
                    for (int channel = 0; channel < 2; ++channel)
                        v.output[(std::size_t)channel][(std::size_t)w.block] = source(v, channel, v.sourcePosition, w.targets, w.originalTarget, pad.loop);
                    if (!advance(v, v.sourcePosition, v.asset->sampleRate / rate * w.directSpeed, pad.reverse, pad.loop)) { ++w.block; break; }
                }
            }
            w.phase = Phase::Mix; return false;
        }
        const auto ringFrames = s.outputMask + 1;
        for (int i = 0; i < w.block; ++i) {
            v.envelope = v.released ? std::max(0.0f, v.envelope - w.release) : std::min(1.0f, v.envelope + w.attack);
            if (v.released && v.envelope <= 0) { v.active = false; break; }
            const double remaining = pad.reverse ? v.logicalPosition - v.start + 1.0 : v.end - v.logicalPosition;
            const float boundaryFade = pad.loop ? 1.0f : (float)std::clamp(remaining / std::max(1.0, w.positionStep * rate * 0.002), 0.0, 1.0);
            const bool mixClick = v.preview && p.metronome;
            const float click = mixClick ? previewClick(v, p) : 0.0f;
            for (int channel = 0; channel < 2; ++channel) {
                const float x = v.output[(std::size_t)channel][(std::size_t)i];
                const float y = v.preview ? x : w.b0 * x + v.z1[(std::size_t)channel];
                if (!v.preview) {
                    v.z1[(std::size_t)channel] = w.b1 * x - w.a1 * y + v.z2[(std::size_t)channel];
                    v.z2[(std::size_t)channel] = w.b0 * x - w.a2 * y;
                }
                const int targetChannel = w.channelOffset + channel;
                if (targetChannel >= 0 && targetChannel < request.channels) {
                    const float dry = y * w.gains[channel] * v.envelope * boundaryFade;
                    s.output[(std::size_t)targetChannel * ringFrames + (std::size_t)((request.start + (std::uint64_t)(job.cursor + w.done + i)) & s.outputMask)] += mixClick ? dry + click * w.gains[channel] : dry;
                }
            }
            if (!advance(v, v.logicalPosition, w.positionStep, pad.reverse, pad.loop) && !pad.loop) { v.active = false; break; }
        }
        w.done += w.block;
        if (!v.stretched && !pad.loop && (pad.reverse ? v.sourcePosition <= v.start - 1.0 : v.sourcePosition >= v.end)) v.active = false;
        if (!v.active || w.done >= count) finishScheduledVoice(v);
        else w.phase = Phase::Pump;
        return false;
    }

    // Returns -1 on a bounded-storage fault, 1 for a process call, 0 otherwise.
    int scheduledStep() noexcept {
        auto& s = scheduled;
        if (s.requestRead == s.requestWrite) return 0;
        auto& job = s.work;
        const auto slot = (std::size_t)(s.requestRead & s.requestMask);
        auto& request = s.requests[slot];
        if (++job.steps > 4194304) return -1;
        if (job.phase == ScheduledWork::Phase::Begin) {
            const auto* view = s.requestHazards[slot].load();
            retained.store(view); audioView = view;
            job.midiCursor = request.midiBegin; job.commandCursor = request.commandBegin;
            job.phase = ScheduledWork::Phase::Commands;
            return 0;
        }
        if (job.phase == ScheduledWork::Phase::Commands) {
            if (job.commandCursor < request.commandEnd) {
                applyCommand(s.commands[(std::size_t)(job.commandCursor++ % scheduledCommandCapacity)], request.parameters);
                return 0;
            }
            job.phase = ScheduledWork::Phase::Segment; return 0;
        }
        if (job.phase == ScheduledWork::Phase::Segment) {
            if (job.midiCursor < request.midiEnd) {
                const auto& item = s.midi[(std::size_t)(job.midiCursor % scheduledMidiCapacity)];
                if (job.cursor < item.offset) {
                    job.segmentEnd = item.offset; job.voiceIndex = 0; job.phase = ScheduledWork::Phase::Voices;
                } else {
                    event(juce::MidiMessage(item.bytes.data(), item.size, 0.0), request.parameters);
                    ++job.midiCursor;
                }
                return 0;
            }
            if (job.cursor < request.frames) {
                job.segmentEnd = request.frames; job.voiceIndex = 0; job.phase = ScheduledWork::Phase::Voices;
                return 0;
            }
            if (s.meterWrite - s.meterRead >= s.meters.size()) return -1;
            auto& meter = s.meters[(std::size_t)(s.meterWrite++ & s.requestMask)];
            meter = {}; meter.end = request.start + (std::uint64_t)request.frames; meter.playhead = s.workPlayhead;
            for (const auto& voice : voices) if (voice.active) {
                meter.anyVoice = true;
                if (voice.preview) meter.preview = true;
                else if (voice.pad >= 0 && voice.pad < padCount)
                    meter.activity[(std::size_t)voice.pad] = std::max(meter.activity[(std::size_t)voice.pad], voice.envelope * voice.velocity);
            }
            s.completedUntil = meter.end;
            s.midiRead = request.midiEnd; s.commandRead = request.commandEnd;
            s.requestHazards[slot].store(nullptr);
            ++s.requestRead; job = {};
            return 0;
        }
        if (job.phase == ScheduledWork::Phase::Voices) {
            while (job.voiceIndex < voiceCount && !voices[(std::size_t)job.voiceIndex].active) ++job.voiceIndex;
            if (job.voiceIndex == voiceCount) {
                job.cursor = job.segmentEnd; job.phase = ScheduledWork::Phase::Segment;
            } else { job.voice = {}; job.phase = ScheduledWork::Phase::Voice; }
            return 0;
        }
        return scheduledVoiceStep(request, voices[(std::size_t)job.voiceIndex]) ? 1 : 0;
    }

    void publishScheduledMeters(std::uint64_t outputEnd) noexcept {
        auto& s = scheduled;
        while (s.meterRead < s.meterWrite) {
            const auto& meter = s.meters[(std::size_t)(s.meterRead & s.requestMask)];
            if (meter.end + (std::uint64_t)s.latency > outputEnd) break;
            playhead.store(meter.playhead, std::memory_order_relaxed);
            previewIsPlaying.store(meter.preview, std::memory_order_relaxed);
            anyVoiceActive.store(meter.anyVoice, std::memory_order_relaxed);
            for (int i = 0; i < padCount; ++i) activity[(std::size_t)i].store(meter.activity[(std::size_t)i], std::memory_order_relaxed);
            ++s.meterRead;
        }
    }
};

SamplerEngine::SamplerEngine() : impl(std::make_unique<Impl>()) {}
SamplerEngine::~SamplerEngine() = default;
void SamplerEngine::prepare(double sampleRate, int) {
    impl->clearNotes();
    impl->rate = std::clamp(sampleRate, 8000.0, 192000.0);
    impl->stemSmoothing = (float)(1.0 - std::exp(-1.0 / (impl->rate * 0.005)));
    for (auto& v : impl->voices) {
        impl->resetVoice(v);
        using R = RubberBand::RubberBandStretcher;
        v.stretcher = std::make_unique<R>((std::size_t) impl->rate, 2, impl->logger,
            R::OptionProcessRealTime | R::OptionThreadingNever | R::OptionWindowShort | R::OptionPitchHighConsistency |
            R::OptionEngineFiner | R::OptionChannelsTogether);
        v.stretcher->setMaxProcessSize(workSize);
        const float* input[] = { v.input[0].data(), v.input[1].data() };
        float* output[] = { v.output[0].data(), v.output[1].data() };
        for (double ratio : { 0.25, 1.0, 4.0 }) for (double pitch : { 0.25, 1.0, 4.0 }) {
            v.stretcher->reset(); v.stretcher->setTimeRatio(ratio); v.stretcher->setPitchScale(pitch);
            for (int i=0; i<8; ++i) {
                const auto count=std::clamp<std::size_t>(v.stretcher->getSamplesRequired(),1,workSize);
                v.stretcher->process(input,count,false);
                while(v.stretcher->available()>0)
                    v.stretcher->retrieve(output,(std::size_t)std::min(workSize,v.stretcher->available()));
            }
        }
        v.stretcher->reset(); v.stretcher->setTimeRatio(1); v.stretcher->setPitchScale(1);
    }
    impl->clearNotes();
}
void SamplerEngine::release() {
    auto& s = impl->scheduled;
    for (auto& v : impl->voices) impl->resetVoice(v);
    impl->clearNotes();
    for (auto i = s.requestRead; i < s.requestWrite; ++i)
        s.requestHazards[(std::size_t)(i & s.requestMask)].store(nullptr);
    s.callHazard.store(nullptr);
    impl->audioView = nullptr; impl->retained.store(nullptr); impl->acquiring.store(nullptr);
    s.ready = false; s.requestRead = s.requestWrite; s.midiRead = s.midiWrite; s.commandRead = s.commandWrite;
    s.meterRead = s.meterWrite; s.work = {};
    impl->previewIsPlaying.store(false); impl->anyVoiceActive.store(false);
}
void SamplerEngine::publish(std::shared_ptr<const SampleAsset> asset, const SessionState& state) {
    auto view = std::make_shared<View>(); view->asset = std::move(asset);
    for (int i = 0; i < padCount; ++i) view->boundaries[(size_t) i] = { state.pads[(size_t) i].startSeconds, state.pads[(size_t) i].endSeconds };
    // Processor validation uses the same limit. Keep direct engine users
    // bounded as well so the audio-thread grid lookup is logarithmic in at
    // most 4096 anchors even if a caller bypasses the processor.
    if (state.beatAnchors.size() <= 4096) view->anchors = state.beatAnchors;
    view->beatOffset = state.global.beatOffsetSeconds;
    {
        std::lock_guard<std::mutex> lock(impl->viewsMutex);
        if (impl->current) impl->retired.push_back(std::move(impl->current));
        impl->current = std::move(view);
        impl->published.store(impl->current.get());
    }
    collectRetired();
}
void SamplerEngine::collectRetired() {
    std::lock_guard<std::mutex> lock(impl->viewsMutex);
    impl->retired.erase(std::remove_if(impl->retired.begin(), impl->retired.end(), [&](const auto& v) {
        const auto& scheduled = impl->scheduled;
        if (scheduled.callHazard.load() == v.get()) return false;
        for (std::size_t i = 0; i < scheduled.hazardCount; ++i)
            if (scheduled.requestHazards[i].load() == v.get()) return false;
        const auto* b = impl->acquiring.load(); const auto* a = impl->retained.load();
        if (v.get() == a || v.get() == b) return false;
        for (const auto& hazard : impl->voiceViews) if (hazard.load() == v.get()) return false;
        return true;
    }), impl->retired.end());
}
void SamplerEngine::enqueueTrigger(int pad, float velocity, bool on) { impl->enqueue({pad, velocity, on}); }
void SamplerEngine::enqueueNote(int note, float velocity, bool on) { impl->enqueue({-4, velocity, on, (double)note}); }
void SamplerEngine::enqueueStop() { impl->enqueue({-1, 0, false, 0}); }
void SamplerEngine::enqueuePreview(double seconds) { impl->enqueue({-2, 1, true, seconds}); }
void SamplerEngine::enqueueStopPreview() { impl->enqueue({-3, 0, false, 0}); }
void SamplerEngine::process(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, const PlaybackParameters& p,
                            const std::array<int, padCount>& offsets) {
    juce::ScopedNoDenormals noDenormals;
    buffer.clear(); impl->updateView();
    const bool discardCommands = impl->commandOverflow.exchange(false, std::memory_order_relaxed);
    if (discardCommands) { for (auto& voice : impl->voices) impl->resetVoice(voice); impl->clearNotes(); }
    const int queued=std::min(commandCount,impl->commandsFifo.getNumReady());
    for (int i=0;i<queued;++i) {
        const auto scope = impl->commandsFifo.read(1);
        if (scope.blockSize1 == 0) break;
        if (discardCommands) continue;
        impl->applyCommand(impl->commands[(size_t) scope.startIndex1], p);
    }
    int cursor = 0;
    for (const auto metadata : midi) {
        const int position = std::clamp(metadata.samplePosition, 0, buffer.getNumSamples());
        impl->render(buffer, cursor, position - cursor, p, offsets);
        if (metadata.numBytes > 0 && metadata.numBytes <= 3 && metadata.data[0] < 0xf0)
            impl->event(metadata.getMessage(), p);
        cursor = position;
    }
    impl->render(buffer, cursor, buffer.getNumSamples() - cursor, p, offsets);
    std::array<float, padCount> active {};
    bool anyVoice = false, preview = false;
    for (const auto& voice : impl->voices) if (voice.active) {
        anyVoice = true;
        if (voice.preview) preview = true;
        else if (voice.pad >= 0 && voice.pad < padCount) active[(size_t)voice.pad] = std::max(active[(size_t)voice.pad], voice.envelope * voice.velocity);
    }
    impl->anyVoiceActive.store(anyVoice, std::memory_order_relaxed);
    impl->previewIsPlaying.store(preview, std::memory_order_relaxed);
    for (int i=0; i<padCount; ++i) impl->activity[(size_t)i].store(active[(size_t)i], std::memory_order_relaxed);
    float peak = 0;
    for (int c = 0; c < buffer.getNumChannels(); ++c) peak = std::max(peak, buffer.getMagnitude(c, 0, buffer.getNumSamples()));
    impl->peak.store(peak, std::memory_order_relaxed);
}

void SamplerEngine::prepareScheduled(double sampleRate, int maximumBlockSize, int latencyFrames) {
    prepare(sampleRate, maximumBlockSize);
    auto& s = impl->scheduled;
    const int maxBlock = std::clamp(maximumBlockSize, 1, 8192);
    const auto nextPow2 = [](std::size_t n) {
        std::size_t result = 1;
        while (result < n) result <<= 1;
        return result;
    };
    const int suggested = (int)std::ceil(0.020 * impl->rate);
    const int wanted = std::max(maxBlock + 1, latencyFrames > 0 ? latencyFrames : suggested);
    const auto latency = (std::size_t)std::clamp(wanted, 1, 262144);
    const auto requestCapacity = nextPow2(latency + (std::size_t)maxBlock + 2);
    const auto outputCapacity = nextPow2(latency + 2 * (std::size_t)maxBlock + 1);
    std::vector<Impl::ScheduledRequest> requests(requestCapacity);
    std::vector<Impl::ScheduledMeter> meters(requestCapacity);
    std::vector<float> output(outputCapacity * scheduledChannels, 0.0f);
    auto hazards = std::make_unique<std::atomic<const View*>[]>(requestCapacity);
    for (std::size_t i = 0; i < requestCapacity; ++i) hazards[i].store(nullptr);
    {
        std::lock_guard<std::mutex> lock(impl->viewsMutex);
        s.callHazard.store(nullptr);
        s.requestHazards = std::move(hazards); s.hazardCount = requestCapacity;
    }
    s.requests = std::move(requests); s.meters = std::move(meters); s.output = std::move(output);
    s.maxBlock = maxBlock; s.latency = (int)latency; s.layoutChannels = -1;
    s.requestMask = requestCapacity - 1; s.outputMask = outputCapacity - 1;
    s.requestRead = s.requestWrite = s.midiRead = s.midiWrite = s.commandRead = s.commandWrite = s.meterRead = s.meterWrite = 0;
    s.streamFrame = s.epochStart = s.completedUntil = 0; s.credits = 0; s.workPlayhead = 0; s.work = {};
    s.ready = true;
    impl->scheduledFaults.store(0, std::memory_order_relaxed);
    (void)std::chrono::steady_clock::now(); // Warm the monotonic clock before audio starts.
}

void SamplerEngine::processScheduled(juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi,
                                     const PlaybackParameters& p, const std::array<int, padCount>& offsets,
                                     bool nonRealtime) noexcept {
    juce::ScopedNoDenormals noDenormals;
    const auto callbackStart = std::chrono::steady_clock::now();
    auto& s = impl->scheduled;
    const int frames = buffer.getNumSamples(), channels = buffer.getNumChannels();
    buffer.clear();
    if (!s.ready) { impl->scheduledFaults.fetch_add(1, std::memory_order_relaxed); return; }
    const auto endFrame = s.streamFrame + (std::uint64_t)frames;
    if (channels > scheduledChannels || (!nonRealtime && frames > s.maxBlock) ||
        impl->commandOverflow.exchange(false, std::memory_order_relaxed)) {
        impl->failScheduled(endFrame); return;
    }
    if (s.layoutChannels >= 0 && (s.layoutChannels != channels || s.layoutOffsets != offsets)) {
        s.layoutChannels = channels; s.layoutOffsets = offsets;
        impl->failScheduled(endFrame); return;
    }
    s.layoutChannels = channels; s.layoutOffsets = offsets;
    if (frames == 0 && midi.isEmpty() && impl->commandsFifo.getNumReady() == 0) return;
    if (!impl->captureScheduledView()) { impl->failScheduled(endFrame); return; }
    impl->deferTelemetry = true;
    int from = 0;
    bool first = true;
    do {
        const int count = std::min(s.maxBlock, frames - from);
        const bool wasIdle = s.requestRead == s.requestWrite;
        if (!impl->submitScheduled(midi, frames, from, count, first, channels, p, offsets)) {
            impl->failScheduled(endFrame); buffer.clear(); impl->deferTelemetry = false; return;
        }
        if (wasIdle) s.credits = 0;
        // Preserve fractional credits for tiny blocks, but never bank idle
        // work. The clock guard is shared by all voices, in addition to this
        // absolute step/feed cap. One already-entered R3 operation is the
        // indivisible worst case, which must be measured on the target CPU.
        s.credits = std::min(std::max(1.0, (double)count), s.credits + count);
        const int maxSteps = std::min(4096, std::max(64, count * 4));
        const auto deadline = callbackStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(0.65 * std::max(1, frames) / impl->rate));
        int steps = 0;
        while (s.requestRead < s.requestWrite && (nonRealtime || steps < maxSteps)) {
            if (!nonRealtime && steps > 0 && std::chrono::steady_clock::now() >= deadline) break;
            bool needsProcess = false;
            auto& work = s.work;
            if (work.phase == Impl::ScheduledWork::Phase::Voice && work.voice.phase == Impl::ScheduledVoiceWork::Phase::Pump) {
                const auto& voice = impl->voices[(std::size_t)work.voiceIndex];
                if (voice.active && voice.stretched && voice.stretcher && !voice.finalSent) {
                    const int available = std::max(0, voice.stretcher->available());
                    needsProcess = !(available > 0 && voice.outputDelay > 0) && (voice.outputDelay > 0 || available <= 0);
                }
            }
            if (!nonRealtime && needsProcess && s.credits < 1.0) break;
            const int result = impl->scheduledStep();
            if (result < 0) { impl->failScheduled(endFrame); buffer.clear(); impl->deferTelemetry = false; return; }
            if (result > 0 && !nonRealtime) s.credits -= 1.0;
            ++steps;
        }
        for (int i = 0; i < count; ++i) {
            const auto outFrame = s.streamFrame + (std::uint64_t)i;
            if (outFrame < s.epochStart + (std::uint64_t)s.latency) continue;
            const auto sourceFrame = outFrame - (std::uint64_t)s.latency;
            if (sourceFrame >= s.completedUntil) {
                impl->failScheduled(endFrame); buffer.clear(); impl->deferTelemetry = false; return;
            }
            for (int channel = 0; channel < channels; ++channel)
                buffer.setSample(channel, from + i, s.output[(std::size_t)channel * (s.outputMask + 1) + (std::size_t)(sourceFrame & s.outputMask)]);
        }
        s.streamFrame += (std::uint64_t)count;
        impl->publishScheduledMeters(s.streamFrame);
        from += count; first = false;
    } while (from < frames);
    s.callHazard.store(nullptr);
    impl->deferTelemetry = false;
    float outputPeak = 0;
    for (int c = 0; c < channels; ++c) outputPeak = std::max(outputPeak, buffer.getMagnitude(c, 0, frames));
    impl->peak.store(outputPeak, std::memory_order_relaxed);
}

int SamplerEngine::scheduledLatencySamples() const noexcept { return impl->scheduled.latency; }
std::uint64_t SamplerEngine::scheduledOverloads() const noexcept { return impl->scheduledFaults.load(std::memory_order_relaxed); }
double SamplerEngine::playheadSeconds() const noexcept { return impl->playhead.load(std::memory_order_relaxed); }
float SamplerEngine::activity(int pad) const noexcept { return pad >= 0 && pad < padCount ? impl->activity[(size_t) pad].load(std::memory_order_relaxed) : 0; }
float SamplerEngine::outputPeak() const noexcept { return impl->peak.load(std::memory_order_relaxed); }
bool SamplerEngine::previewPlaying() const noexcept { return impl->previewIsPlaying.load(std::memory_order_relaxed); }
bool SamplerEngine::voicesActive() const noexcept { return impl->anyVoiceActive.load(std::memory_order_relaxed); }
std::uint64_t SamplerEngine::commandOverflows() const noexcept { return impl->overflowCount.load(std::memory_order_relaxed); }
}
