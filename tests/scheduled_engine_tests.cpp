#include "SamplerEngine.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <rubberband/RubberBandStretcher.h>
#include <cstring>
#include <vector>

using namespace slicelab;
namespace {
int failures = 0;
void check(bool ok, const char* text) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << text << '\n'; }
    else std::cout << "PASS: " << text << '\n';
}
std::shared_ptr<SampleAsset> sample(float sign = 1.0f) {
    auto a = std::make_shared<SampleAsset>(); a->sampleRate = 48000; a->audio.setSize(2, 96000);
    for (int i = 0; i < a->audio.getNumSamples(); ++i) {
        const auto x = sign * 0.5f * std::sin((float)(2.0 * juce::MathConstants<double>::pi * 440.0 * i / 48000.0));
        a->audio.setSample(0, i, x); a->audio.setSample(1, i, x);
    }
    return a;
}
SessionState state() {
    SessionState result;
    for (auto& pad : result.pads) { pad.startSeconds = 0; pad.endSeconds = 2; }
    return result;
}
PlaybackParameters parameters() {
    PlaybackParameters p; p.sync = false; p.sourceBpm = p.targetBpm = 120; p.gainDb = 0;
    p.hold = true; p.pads[0].loop = true; p.pads[0].attackMs = p.pads[0].releaseMs = 0;
    return p;
}
float magnitude(const std::vector<float>& audio, int begin, int end) {
    float value = 0; for (int i = begin; i < end && i < (int)audio.size(); ++i) value = std::max(value, std::abs(audio[(std::size_t)i]));
    return value;
}
struct Render { std::vector<float> pcm; int latency = 0; std::uint64_t overloads = 0; double maxCallbackUs = 0; };
Render render(bool scheduled, bool offline, bool stretched, bool repitch, const std::vector<int>& blocks,
              int requestedLatency = 4096, bool switchModes = false) {
    SamplerEngine engine;
    if (scheduled) engine.prepareScheduled(48000, 128, requestedLatency); else engine.prepare(48000, 128);
    engine.publish(sample(), state());
    auto p = parameters(); p.repitch = repitch;
    if (stretched || repitch) { p.pads[0].semitones = 7; p.pads[0].timeRatio = 1.25f; }
    const int latency = scheduled ? engine.scheduledLatencySamples() : 0;
    const int total = 12000 + latency;
    std::array<int, padCount> offsets {};
    std::array<std::array<float, 2048>, 2> storage {};
    float* pointers[] = {storage[0].data(), storage[1].data()};
    juce::MidiBuffer midi; midi.ensureSize(1024);
    Render result; result.latency = latency; result.pcm.reserve((std::size_t)total);
    int start = 0, index = 0;
    while (start < total) {
        const int count = std::min({blocks[(std::size_t)(index++ % (int)blocks.size())], 2048, total - start});
        juce::AudioBuffer<float> buffer(pointers, 2, count); midi.clear();
        if (37 >= start && 37 < start + count) midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 37 - start);
        if (1200 >= start && 1200 < start + count) midi.addEvent(juce::MidiMessage::noteOff(1, 36), 1200 - start);
        const auto begin = std::chrono::steady_clock::now();
        if (scheduled) engine.processScheduled(buffer, midi, p, offsets, switchModes ? (index % 7 == 0) : offline);
        else engine.process(buffer, midi, p, offsets);
        const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
        result.maxCallbackUs = std::max(result.maxCallbackUs, elapsed);
        result.pcm.insert(result.pcm.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + count);
        start += count;
    }
    result.overloads = engine.scheduledOverloads();
    return result;
}
void compareDelay(bool repitch) {
    const std::vector<int> blocks { 128, 73, 17, 128, 41, 64 };
    const auto reference = render(false, false, false, repitch, blocks);
    const auto delayed = render(true, false, false, repitch, blocks);
    double largest = 0;
    for (std::size_t i = 0; i < reference.pcm.size() && i + (std::size_t)delayed.latency < delayed.pcm.size(); ++i)
        largest = std::max(largest, (double)std::abs(reference.pcm[i] - delayed.pcm[i + (std::size_t)delayed.latency]));
    check(delayed.overloads == 0 && largest < 1e-6, repitch ? "repitch output has exactly the reported delay" : "neutral output has exactly the reported delay");
    check(magnitude(delayed.pcm, 0, delayed.latency + 37) == 0 && magnitude(delayed.pcm, delayed.latency + 60, delayed.latency + 900) > 0.1f,
          "sample-offset onset survives the fixed delay");
    check(magnitude(delayed.pcm, delayed.latency + 1250, delayed.latency + 2000) < 1e-6f, "note-off occurs at its delayed sample time");
}
void stretchedModes() {
    const std::vector<int> blocks {128};
    const auto offline = render(true, true, true, false, blocks);
    const auto realtime = render(true, false, true, false, blocks);
    const auto mixed = render(true, false, true, false, blocks, 4096, true);
    double error = 0, modeError = 0;
    for (std::size_t i = 0; i < realtime.pcm.size(); ++i) {
        error = std::max(error, (double)std::abs(realtime.pcm[i] - offline.pcm[i]));
        modeError = std::max(modeError, (double)std::abs(realtime.pcm[i] - mixed.pcm[i]));
    }
    check(realtime.overloads == 0 && error < 1e-6, "R3 realtime and offline preserve the same delayed PCM");
    check(mixed.overloads == 0 && modeError < 1e-6, "switching realtime and offline preserves delayed state");
    check(magnitude(realtime.pcm, realtime.latency + 40, realtime.latency + 1100) > 0.02f,
          "note-off cannot erase older stretched PCM that was still being primed");
    std::cout << "Measured 128-frame R3 callback maximum (us): " << realtime.maxCallbackUs << '\n';
}
void viewAndAutomationCapture() {
    SamplerEngine engine; engine.prepareScheduled(48000, 128, 4096);
    auto old = sample(); std::weak_ptr<SampleAsset> oldWeak = old; engine.publish(old, state());
    auto p = parameters(); p.mono = true;
    std::array<int, padCount> offsets {}; juce::AudioBuffer<float> buffer(2, 128); juce::MidiBuffer midi; midi.ensureSize(8192);
    for (int i = 0; i < 400; ++i) midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 0);
    engine.processScheduled(buffer, midi, p, offsets, false);
    old.reset(); engine.publish(sample(-1), state()); engine.collectRetired();
    check(!oldWeak.expired(), "queued request pins the view captured at intake");
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 0); p.gainDb = -12;
    engine.processScheduled(buffer, midi, p, offsets, false);
    std::vector<float> output; output.reserve(8192);
    output.insert(output.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 128);
    midi.clear();
    for (int i = 0; i < 36; ++i) {
        engine.processScheduled(buffer, midi, p, offsets, false);
        output.insert(output.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 128);
    }
    const int first = engine.scheduledLatencySamples() - 128;
    check(engine.scheduledOverloads() == 0 && magnitude(output, first + 20, first + 120) > 0.3f &&
          magnitude(output, first + 148, first + 248) < 0.2f,
          "queued note uses its original view and gain before the next request");
    engine.collectRetired();
    check(oldWeak.expired(), "completed request and replaced voice release the old asset");
}
void failureAndOversize() {
    SamplerEngine engine; engine.prepareScheduled(48000, 128, 4096); engine.publish(sample(), state());
    auto p = parameters(); std::array<int, padCount> offsets {}; juce::AudioBuffer<float> buffer(2, 128); juce::MidiBuffer midi; midi.ensureSize(40000);
    for (int i = 0; i < 4097; ++i) midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 0);
    engine.processScheduled(buffer, midi, p, offsets, false);
    check(engine.scheduledOverloads() == 1 && buffer.getMagnitude(0, 0, 128) == 0, "MIDI overflow mutes and records an overload");
    midi.clear();
    for (int i = 0; i < 40; ++i) engine.processScheduled(buffer, midi, p, offsets, false);
    check(buffer.getMagnitude(0, 0, 128) == 0 && !engine.voicesActive(), "recovery never replays notes from the failed epoch");
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 0);
    engine.processScheduled(buffer, midi, p, offsets, false); midi.clear(); float peak = 0;
    for (int i = 0; i < 40; ++i) { engine.processScheduled(buffer, midi, p, offsets, false); peak = std::max(peak, buffer.getMagnitude(0, 0, 128)); }
    check(peak > 0.1f && engine.scheduledOverloads() == 1, "new notes play after a full-delay recovery");
    juce::AudioBuffer<float> oversize(2, 256); engine.processScheduled(oversize, midi, p, offsets, false);
    check(engine.scheduledOverloads() == 2 && oversize.getMagnitude(0, 0, 256) == 0, "oversized realtime blocks fail silent");
    offsets[1] = 2;
    engine.processScheduled(buffer, midi, p, offsets, false);
    check(engine.scheduledOverloads() == 3 && buffer.getMagnitude(0, 0, 128) == 0,
          "a changed bus map clears old queued output");
    const auto split = render(true, true, false, false, {128});
    const auto large = render(true, true, false, false, {2048});
    double error = 0;
    for (std::size_t i = 0; i < split.pcm.size(); ++i) error = std::max(error, (double)std::abs(split.pcm[i] - large.pcm[i]));
    check(large.overloads == 0 && error < 1e-6, "oversized offline blocks split without changing PCM or delay");
}
void independentDurationAndPitch() {
    SamplerEngine engine; engine.prepareScheduled(48000, 128, 4096);
    auto slices = state(); slices.pads[0].endSeconds = .5; engine.publish(sample(), slices);
    auto p = parameters(); p.hold = false; p.pads[0].loop = false; p.pads[0].timeRatio = 2; p.pads[0].semitones = 12;
    juce::AudioBuffer<float> buffer(2, 128); juce::MidiBuffer midi; std::array<int, padCount> offsets {};
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 0);
    const int latency = engine.scheduledLatencySamples(); std::vector<float> out; out.reserve(65000);
    for (int block = 0; block < 490; ++block) {
        engine.processScheduled(buffer, midi, p, offsets, false); midi.clear();
        out.insert(out.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 128);
    }
    int crossings = 0, last = 0;
    for (int i = latency + 4800; i < latency + 40000; ++i) if (out[(std::size_t)i-1] <= 0 && out[(std::size_t)i] > 0) ++crossings;
    for (int i = 0; i < (int)out.size(); ++i) if (std::abs(out[(std::size_t)i]) > .01f) last = i;
    const double frequency = crossings * 48000.0 / (40000 - 4800);
    check(engine.scheduledOverloads() == 0 && std::abs(frequency - 880) < 35 && last > latency + 43000 && last < latency + 50000,
          "R3 doubles duration and raises pitch independently through its bounded tail");
}
void shortSlice() {
    SamplerEngine engine; engine.prepareScheduled(48000, 128, 4096);
    auto slices = state(); slices.pads[0].startSeconds = 100.0 / 48000.0; slices.pads[0].endSeconds = 101.0 / 48000.0;
    engine.publish(sample(), slices); auto p = parameters(); p.pads[0].loop = false; p.pads[0].semitones = 12;
    juce::AudioBuffer<float> buffer(2, 128); juce::MidiBuffer midi; std::array<int, padCount> offsets {};
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 0);
    bool finite = true;
    for (int block = 0; block < 40; ++block) {
        engine.processScheduled(buffer, midi, p, offsets, false); midi.clear();
        for (int i = 0; i < 128; ++i) finite = finite && std::isfinite(buffer.getSample(0, i));
    }
    check(finite && engine.scheduledOverloads() == 0 && !engine.voicesActive(), "one-frame stretched slice ends using bounded zero feeding");
}
void enableLoopDuringTail() {
    SamplerEngine engine; engine.prepareScheduled(48000, 128, 960);
    auto slices = state(); slices.pads[0].endSeconds = .005; engine.publish(sample(), slices);
    auto p = parameters(); p.pads[0].loop = false; p.pads[0].semitones = 12;
    juce::AudioBuffer<float> buffer(2, 128); juce::MidiBuffer midi; std::array<int, padCount> offsets {};
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 0);
    engine.processScheduled(buffer, midi, p, offsets, true); midi.clear(); p.pads[0].loop = true;
    float later = 0;
    for (int i = 0; i < 110; ++i) {
        engine.processScheduled(buffer, midi, p, offsets, true);
        if (i >= 90) later = std::max(later, buffer.getMagnitude(0, 0, 128));
    }
    check(engine.scheduledOverloads() == 0 && later > .02f && engine.voicesActive(),
          "enabling loop during a stretched tail resumes source feeding");
}
void reverseFractionalDurations() {
    bool directOk = true, scheduledOk = true;
    for (const auto& rates : {std::array<int, 2>{48000, 48000}, {44100, 48000}, {48000, 96000}, {96000, 48000}}) {
        const double sourceRate = rates[0], outputRate = rates[1];
        auto a = std::make_shared<SampleAsset>(); a->sampleRate = sourceRate; a->audio.setSize(2, 16);
        for (int channel = 0; channel < 2; ++channel) for (int i = 0; i < 16; ++i) a->audio.setSample(channel, i, .7f);
        for (bool scheduled : {false, true}) {
            SamplerEngine engine;
            if (scheduled) engine.prepareScheduled(outputRate, 1, 8); else engine.prepare(outputRate, 1);
            const int latency = scheduled ? engine.scheduledLatencySamples() : 0;
            juce::AudioBuffer<float> buffer(2, 1); juce::MidiBuffer midi; std::array<int, padCount> offsets {};
            for (bool repitch : {false, true}) for (double ratio : {.25, 1., 4.}) for (int sourceFrames : {1, 2, 5}) {
                auto settings = state(); settings.pads[0].startSeconds = 2 / sourceRate;
                settings.pads[0].endSeconds = (2 + sourceFrames) / sourceRate;
                auto p = parameters(); p.hold = false; p.repitch = repitch; p.pads[0].loop = false;
                p.pads[0].timeRatio = (float)ratio;
                const int expected = (int)std::ceil(sourceFrames * outputRate * ratio / sourceRate - 1.0e-10);
                for (bool reverse : {false, true}) {
                    p.pads[0].reverse = reverse; engine.publish(a, settings);
                    midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 0);
                    int actual = 0, lastAudible = -1; float renderedPeak = 0;
                    for (int i = 0; i < latency + expected + 64; ++i) {
                        if (scheduled) engine.processScheduled(buffer, midi, p, offsets, true);
                        else engine.process(buffer, midi, p, offsets);
                        midi.clear();
                        if (i >= latency) {
                            const float value = std::abs(buffer.getSample(0, 0));
                            renderedPeak = std::max(renderedPeak, value);
                            if (value > 1.0e-12f) lastAudible = i - latency;
                        }
                        if (i >= latency && !engine.voicesActive()) { actual = i - latency + 1; break; }
                    }
                    if (scheduled) engine.processScheduled(buffer, midi, p, offsets, true);
                    else engine.process(buffer, midi, p, offsets);
                    const float after = std::abs(buffer.getSample(0, 0));
                    if (actual != expected || lastAudible != expected - 1 || after > 1.0e-12f ||
                        (scheduled && engine.scheduledOverloads() != 0)) {
                        std::cerr << "duration source_rate=" << sourceRate << " output_rate=" << outputRate
                                  << " source_frames=" << sourceFrames << " ratio=" << ratio << " repitch=" << repitch
                                  << " reverse=" << reverse << " scheduled=" << scheduled << " expected=" << expected
                                  << " actual=" << actual << " last_audible=" << lastAudible << " peak=" << renderedPeak
                                  << " after=" << after << '\n';
                        (scheduled ? scheduledOk : directOk) = false;
                    }
                }
            }
        }
    }
    check(directOk, "direct forward and reverse frame counts agree with source duration across rates and ratios");
    check(scheduledOk, "scheduled forward and reverse frame counts agree with source duration across rates and ratios");
}
void zeroAndPoly(bool stretched) {
    SamplerEngine engine; engine.prepareScheduled(48000, 128, 0); engine.publish(sample(), state());
    check(engine.scheduledLatencySamples() == 960, "default reports exactly 20 ms at 48 kHz");
    auto p = parameters(); p.mono = false;
    std::array<int, padCount> offsets {}; juce::AudioBuffer<float> empty(2, 0), buffer(2, 128); juce::MidiBuffer midi;
    for (int i = 0; i < padCount; ++i) { p.pads[(std::size_t)i].loop = true; p.pads[(std::size_t)i].attackMs = 0; p.pads[(std::size_t)i].semitones=stretched ? 7.0f : 0.0f; midi.addEvent(juce::MidiMessage::noteOn(1, 36 + i, (juce::uint8)127), 0); }
    engine.processScheduled(empty, midi, p, offsets, true); midi.clear();
    for (int i = 0; i < 34; ++i) engine.processScheduled(buffer, midi, p, offsets, true);
    bool all = true; for (int i = 0; i < padCount; ++i) all = all && engine.activity(i) > 0;
    check(engine.scheduledOverloads() == 0 && all && buffer.getMagnitude(0,0,128)>0.1f, stretched ? "all 32 R3 voices render offline without losing zero-frame MIDI" : "zero-frame MIDI is ordered and all 32 neutral voices remain available");
}

void benchmarkIndivisible() {
    using R = RubberBand::RubberBandStretcher;
    float left[128] {}, right[128] {}, outL[1024] {}, outR[1024] {};
    const float* input[] { left, right }; float* output[] { outL, outR };
    for (int rate : {48000, 96000}) for (double ratio : {0.25, 1.0, 4.0}) for (double pitch : {0.25, 1.0, 4.0}) {
        R r((std::size_t)rate, 2, R::OptionProcessRealTime | R::OptionThreadingNever | R::OptionWindowShort |
            R::OptionPitchHighConsistency | R::OptionEngineFiner | R::OptionChannelsTogether);
        r.setMaxProcessSize(1024); r.setTimeRatio(ratio); r.setPitchScale(pitch);
        std::vector<double> calls; calls.reserve(512);
        for (int i = 0; i < 512; ++i) {
            for (int j = 0; j < 128; ++j) left[j] = right[j] = std::sin((i * 128 + j) * 0.03f);
            const auto begin = std::chrono::steady_clock::now(); r.process(input, 128, false);
            const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
            calls.push_back(us);
            while (r.available() > 0) r.retrieve(output, (std::size_t)std::min(1024, r.available()));
        }
        std::sort(calls.begin(), calls.end());
        std::cout << "indivisible rate=" << rate << " ratio=" << ratio << " pitch=" << pitch
                  << " p99_us=" << calls[506] << " max_us=" << calls.back() << '\n';
    }
}
void benchmarkScheduled(int frames, int voices, bool stretched, int latency, double ratio = 1, double pitch = 1.4983070769, int rate = 48000, int warmFrames = 0) {
    SamplerEngine engine; engine.prepareScheduled(rate, frames, latency); engine.publish(sample(), state());
    auto p = parameters(); p.mono = false;
    for (int i = 0; i < voices; ++i) {
        p.pads[(std::size_t)i].loop = true; p.pads[(std::size_t)i].attackMs = 0;
        if (stretched) { p.pads[(std::size_t)i].semitones = (float)(12 * std::log2(pitch)); p.pads[(std::size_t)i].timeRatio = (float)ratio; }
    }
    juce::AudioBuffer<float> buffer(2, frames); juce::MidiBuffer midi; midi.ensureSize(1024); std::array<int, padCount> offsets {};
    for (int i = 0; i < voices; ++i) midi.addEvent(juce::MidiMessage::noteOn(1, 36 + i, (juce::uint8)127), 0);
    for (int done = 0; done < warmFrames; done += frames) {
        engine.processScheduled(buffer, midi, p, offsets, true); midi.clear();
    }
    std::vector<double> calls; calls.reserve(300); float peak = 0; int firstFault = -1;
    for (int i = 0; i < 300; ++i) {
        const auto begin = std::chrono::steady_clock::now(); engine.processScheduled(buffer, midi, p, offsets, false);
        const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
        calls.push_back(us); midi.clear(); peak = std::max(peak, buffer.getMagnitude(0, 0, frames));
        if (firstFault < 0 && engine.scheduledOverloads() > 0) firstFault = i;
    }
    std::sort(calls.begin(), calls.end());
    std::cout << "scheduled rate=" << rate << " frames=" << frames << " voices=" << voices << " stretched=" << stretched
              << " ratio=" << ratio << " pitch=" << pitch << " latency=" << engine.scheduledLatencySamples() << " p99_us=" << calls[296]
              << " max_us=" << calls.back() << " overloads=" << engine.scheduledOverloads() << " peak=" << peak
              << " warm_frames=" << warmFrames << " first_fault_callback=" << firstFault << '\n';
}
}
int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--reverse-duration") == 0) {
        reverseFractionalDurations();
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "--warm") == 0) {
        for (int voices : {2, 4, 8, 32}) benchmarkScheduled(512, voices, true, 960, 1, 1.4983070769, 48000, 9600);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--poly") == 0) {
        for (int frames : {128, 512}) for (int voices : {2, 4, 8, 16, 32}) benchmarkScheduled(frames, voices, true, 960);
        for (int latency : {576, 768, 960}) benchmarkScheduled(128, 1, true, latency);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--rates") == 0) {
        for (int rate : {44100, 96000}) for (int frames : {128, 512}) for (double ratio : {0.25, 1.0, 4.0}) for (double pitch : {0.25, 1.0, 4.0})
            if (ratio != 1.0 || pitch != 1.0) benchmarkScheduled(frames, 1, true, rate / 50, ratio, pitch, rate);
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--benchmark") == 0) {
        benchmarkIndivisible();
        for (int frames : {128, 512}) for (int voices : {1, 32}) for (bool stretch : {false, true})
            benchmarkScheduled(frames, voices, stretch, 960);
        for (int frames : {128, 512}) for (double ratio : {0.25, 1.0, 4.0}) for (double pitch : {0.25, 1.0, 4.0})
            if (ratio != 1.0 || pitch != 1.0) benchmarkScheduled(frames, 1, true, 960, ratio, pitch);
        return 0;
    }
    compareDelay(false); compareDelay(true); stretchedModes(); viewAndAutomationCapture(); failureAndOversize(); independentDurationAndPitch(); shortSlice(); enableLoopDuringTail(); reverseFractionalDurations(); zeroAndPoly(false); zeroAndPoly(true);
    std::cout << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
