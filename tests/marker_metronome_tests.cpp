#include "MarkerImport.h"
#include "PluginProcessor.h"
#include "SamplerEngine.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace slicelab;
namespace {
int failures = 0;
void check(bool condition, const char* description) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << description << '\n';
    if (!condition) ++failures;
}

void parserTests() {
    std::vector<Marker> markers {{ 9, "unchanged" }};
    auto status = parseMarkers(juce::String::fromUTF8("seconds,name\n1.5,\"Later, loud\"\n0.5,\"śpiew \"\"A\"\"\"\n"), 2, markers);
    check(status.wasOk() && markers.size() == 2 && markers[0].seconds == .5 && markers[1].seconds == 1.5 &&
          markers[0].name == juce::String::fromUTF8("śpiew \"A\"") && markers[1].name == "Later, loud",
          "CSV headers, quoted commas, doubled quotes, Unicode and sorting");
    status = parseMarkers("1.2\tTwo, with comma\r\n0\tOne\r\n", 2, markers);
    check(status.wasOk() && markers.size() == 2 && markers[0].seconds == 0 && markers[1].name == "Two, with comma",
          "headerless TSV accepts CRLF and commas in names");
    const std::vector<juce::String> invalid {
        "", "seconds,name\n", "0.5\n", "0.5,One,extra\n", "0.5,\"unclosed\n", "0.5,\"x\"bad\n",
        "0.5,good\n1.0\twrong delimiter\n", "nan,no\n", "inf,no\n", "0.5garbage,no\n", "-0.01,no\n",
        "2,no\n", "0.5,first\n0.5,duplicate\n", "name,seconds\n", "0.5,good\n1.0,\"bad\nname\"\n"
    };
    bool rejected = true;
    for (const auto& text : invalid) {
        std::vector<Marker> before {{ 9, "unchanged" }};
        rejected &= parseMarkers(text, 2, before).failed() && before.size() == 1 && before[0].seconds == 9 && before[0].name == "unchanged";
    }
    check(rejected, "malformed, nonfinite, duplicate and out-of-range data never returns partial markers");
    auto name40 = juce::String::repeatedString(juce::String::fromUTF8("ż"), 40);
    check(parseMarkers("0," + name40, 2, markers).wasOk() && markers[0].name == name40 &&
          parseMarkers("0," + name40 + juce::String::fromUTF8("ż"), 2, markers).failed(),
          "UTF-8 name limit counts bytes without splitting characters");
    juce::String tooMany;
    for (int i = 0; i < 33; ++i) tooMany += juce::String(i * .01, 2) + ",x\n";
    check(parseMarkers(tooMany, 2, markers).failed(), "more than 32 markers are rejected");
}

constexpr int sampleRate = 8000;
std::shared_ptr<SampleAsset> source(bool tone = false) {
    auto result = std::make_shared<SampleAsset>();
    result->sampleRate = sampleRate; result->audio.setSize(2, sampleRate * 4); result->audio.clear();
    if (tone) for (int i = 0; i < result->audio.getNumSamples(); ++i)
        for (int c = 0; c < 2; ++c) result->audio.setSample(c, i, .4f * std::sin((float)(2 * juce::MathConstants<double>::pi * 220 * i / sampleRate)));
    return result;
}
PlaybackParameters params() {
    PlaybackParameters p; p.sync = false; p.sourceBpm = p.targetBpm = 120; p.gainDb = 0; p.repitch = true;
    return p;
}
struct Audio { std::vector<float> pcm; int latency = 0; std::uint64_t overloads = 0; };
Audio preview(bool scheduled, const SessionState& state, const PlaybackParameters& p, double start,
              int frames, const std::vector<int>& blocks = {64}, bool audibleSource = false) {
    SamplerEngine engine;
    if (scheduled) engine.prepareScheduled(sampleRate, 128, 512); else engine.prepare(sampleRate, 128);
    engine.publish(source(audibleSource), state); engine.enqueuePreview(start);
    Audio result; result.latency = scheduled ? engine.scheduledLatencySamples() : 0;
    std::array<int, padCount> offsets {}; juce::MidiBuffer midi;
    int position = 0, index = 0;
    while (position < frames + result.latency) {
        const int count = std::min(blocks[(std::size_t)(index++ % (int)blocks.size())], frames + result.latency - position);
        juce::AudioBuffer<float> buffer(2, count);
        if (scheduled) engine.processScheduled(buffer, midi, p, offsets, true); else engine.process(buffer, midi, p, offsets);
        result.pcm.insert(result.pcm.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + count);
        position += count;
    }
    result.overloads = engine.scheduledOverloads();
    return result;
}
std::vector<int> clicks(const std::vector<float>& samples, int latency = 0) {
    std::vector<int> result;
    int last = -sampleRate;
    for (int i = latency; i < (int)samples.size(); ++i) if (std::abs(samples[(std::size_t)i]) > .01f) {
        if (i - last > sampleRate / 20) result.push_back(i - latency);
        last = i;
    }
    return result;
}
bool nearby(int actual, int expected, int tolerance = 2) { return std::abs(actual - expected) <= tolerance; }
void metronomeTests() {
    SessionState state; state.global.beatOffsetSeconds = .25;
    state.beatAnchors = {{0, 0}, {1, 2}, {2, 3}};
    auto p = params(); p.metronome = true;
    const auto direct = preview(false, state, p, 0, sampleRate * 3, {64});
    const auto starts = clicks(direct.pcm);
    check(starts.size() >= 5 && nearby(starts[0], 2000) && nearby(starts[1], 6000) && nearby(starts[2], 10000) &&
          nearby(starts[3], 14024, 32) && nearby(starts[4], 18024, 32),
          "source offset and piecewise beat anchors place preview clicks at source-grid crossings");
    check(starts.size() >= 2 && std::abs(direct.pcm[(std::size_t)starts[0]] - .08f) < .001f &&
          std::abs(direct.pcm[(std::size_t)starts[1]] - .05f) < .001f &&
          std::abs(direct.pcm[(std::size_t)starts[0] + 64]) < .000001f,
          "four-beat accents and the eight-millisecond click tail");
    const auto shifted = preview(false, state, p, .25, 1000);
    check(std::abs(shifted.pcm[0] - .08f) < .001f, "seeking exactly to a grid line clicks on the first preview sample");
    const auto after = preview(false, state, p, .250125, 4500);
    const auto afterClicks = clicks(after.pcm);
    check(afterClicks.size() == 1 && nearby(afterClicks[0], 3999), "seeking just after a beat waits for the next grid line");
    p.gainDb = -6;
    const auto quiet = preview(false, state, p, .25, 128);
    check(std::abs(quiet.pcm[0] - .08f * std::pow(10.0f, -6.0f / 20)) < .001f, "master gain scales the click");
    p.gainDb = 0; p.semitones = 12;
    SessionState simple; simple.global.beatOffsetSeconds = .25;
    const auto pitched = preview(false, simple, p, 0, 7000);
    const auto pitchClicks = clicks(pitched.pcm);
    check(pitchClicks.size() >= 3 && nearby(pitchClicks[0], 1000) && nearby(pitchClicks[1], 3000) && nearby(pitchClicks[2], 5000),
          "repitch tempo changes keep clicks aligned to source time");
    p.semitones = 0;
    const std::vector<int> blocks {64, 37, 23, 101, 59};
    const auto a = preview(false, state, p, 0, sampleRate * 3, blocks);
    const auto b = preview(true, state, p, 0, sampleRate * 3, blocks);
    bool identical = b.overloads == 0 && b.pcm.size() >= a.pcm.size() + (std::size_t)b.latency;
    for (std::size_t i = 0; identical && i < a.pcm.size(); ++i) identical &= a.pcm[i] == b.pcm[i + (std::size_t)b.latency];
    check(identical, "scheduled and direct preview clicks are sample-identical after reported latency");
    auto stretched = p; stretched.repitch = false; stretched.semitones = 12; stretched.targetBpm = 60;
    const auto stretchDirect = preview(false, simple, stretched, 0, sampleRate * 3, blocks);
    const auto stretchScheduled = preview(true, simple, stretched, 0, sampleRate * 3, blocks);
    const auto stretchClicks = clicks(stretchDirect.pcm);
    bool stretchIdentical = stretchScheduled.overloads == 0 && stretchScheduled.pcm.size() >= stretchDirect.pcm.size() + (std::size_t)stretchScheduled.latency;
    for (std::size_t i = 0; stretchIdentical && i < stretchDirect.pcm.size(); ++i)
        stretchIdentical &= stretchDirect.pcm[i] == stretchScheduled.pcm[i + (std::size_t)stretchScheduled.latency];
    check(stretchIdentical && stretchClicks.size() >= 3 && nearby(stretchClicks[0], 4000) && nearby(stretchClicks[1], 12000) && nearby(stretchClicks[2], 20000),
          "independent pitch and tempo changes keep clicks aligned after latency");
    auto padMutated = p; padMutated.pads[0].gainDb = -60; padMutated.pads[0].pan = 1; padMutated.pads[0].reverse = true;
    padMutated.pads[0].semitones = -24; padMutated.pads[0].timeRatio = 4; padMutated.pads[0].loop = true;
    const auto independent = preview(false, state, padMutated, 0, sampleRate, {64});
    const auto unmodified = preview(false, state, p, 0, sampleRate, {64});
    check(independent.pcm == unmodified.pcm, "source preview clicks do not use pad controls");
    SamplerEngine engine; engine.prepare(sampleRate, 128); engine.publish(source(), state);
    std::array<int, padCount> offsets {}; juce::AudioBuffer<float> buffer(2, 64); juce::MidiBuffer midi;
    engine.enqueuePreview(.25); engine.process(buffer, midi, p, offsets); engine.enqueueStopPreview(); engine.process(buffer, midi, p, offsets);
    check(buffer.getMagnitude(0, 0, 64) == 0, "stopping preview stops the click tail");
    engine.enqueuePreview(.25); engine.process(buffer, midi, p, offsets);
    check(std::abs(buffer.getSample(0, 0) - .08f) < .001f, "a new preview resets its first beat and click tail");
    SamplerEngine scheduled; scheduled.prepareScheduled(sampleRate, 64, 512); scheduled.publish(source(), state);
    std::vector<float> transitions;
    for (int i = 0; i < 12; ++i) {
        if (i == 0 || i == 2) scheduled.enqueuePreview(.25);
        if (i == 1) scheduled.enqueueStopPreview();
        scheduled.processScheduled(buffer, midi, p, offsets, true);
        transitions.insert(transitions.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 64);
    }
    const int delay = scheduled.scheduledLatencySamples();
    check(scheduled.scheduledOverloads() == 0 && std::abs(transitions[(std::size_t)delay] - .08f) < .001f &&
          std::abs(transitions[(std::size_t)delay + 64]) < .000001f &&
          std::abs(transitions[(std::size_t)delay + 128] - .08f) < .001f,
          "queued stop and new preview reset click state at their delayed sample times");
}
std::vector<float> renderPad(bool metro, bool scheduled) {
    SamplerEngine engine; if (scheduled) engine.prepareScheduled(sampleRate, 128, 512); else engine.prepare(sampleRate, 128); SessionState state;
    state.pads[0].startSeconds = 0; state.pads[0].endSeconds = 2; engine.publish(source(true), state);
    auto p = params(); p.metronome = metro; p.pads[0].attackMs = 0;
    std::array<int, padCount> offsets {}; juce::AudioBuffer<float> buffer(2, 128); juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 36, (juce::uint8)127), 37);
    std::vector<float> pcm;
    for (int i = 0; i < 32; ++i) {
        if (scheduled) engine.processScheduled(buffer, midi, p, offsets, true); else engine.process(buffer, midi, p, offsets);
        midi.clear();
        pcm.insert(pcm.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + 128);
    }
    return pcm;
}
void offAndPadTests() {
    check(renderPad(false, false) == renderPad(true, false) && renderPad(false, true) == renderPad(true, true),
          "metronome never alters pad playback without a preview, in either renderer");
    SessionState state; auto p = params();
    const auto dryA = preview(false, state, p, 0, 4096, {64}, true);
    p.metronome = true; const auto wet = preview(false, state, p, 0, 4096, {64}, true);
    p.metronome = false; const auto dryB = preview(false, state, p, 0, 4096, {64}, true);
    check(dryA.pcm == dryB.pcm && dryA.pcm != wet.pcm, "default-off preview produces stable unchanged PCM");
}

bool samePad(const PadSettings& a, const PadSettings& b) {
    return a.startSeconds == b.startSeconds && a.endSeconds == b.endSeconds && a.name == b.name && a.favorite == b.favorite &&
           a.gainDb == b.gainDb && a.pan == b.pan && a.semitones == b.semitones && a.loop == b.loop && a.reverse == b.reverse;
}
void legacyStateTests(const juce::MemoryBlock& saved, const Snapshot& expected, SliceLabAudioProcessor& restored) {
    auto legacy = juce::AudioProcessor::getXmlFromBinary(saved.getData(), (int)saved.getSize());
    auto* parameters = legacy ? legacy->getChildByName("PARAMETERS") : nullptr;
    juce::XmlElement* metro = nullptr;
    if (parameters) for (auto* child : parameters->getChildIterator())
        if (child->getStringAttribute("id") == "metronome") { metro = child; break; }
    const bool fullState = legacy && legacy->getIntAttribute("schema") == 1 && parameters &&
                           parameters->getNumChildElements() == 411 && metro;
    if (metro) parameters->removeChildElement(metro, true);
    check(fullState && parameters->getNumChildElements() == 410,
          "legacy fixture contains exactly the 410 original schema-1 parameters");
    if (!fullState || parameters->getNumChildElements() != 410) return;

    juce::MemoryBlock oldState; juce::AudioProcessor::copyXmlToBinary(*legacy, oldState);
    const auto* beforeMetro = restored.parameters.getRawParameterValue("metronome");
    const bool wasOn = beforeMetro && beforeMetro->load() > .5f;
    restored.setStateInformation(oldState.getData(), (int)oldState.getSize());
    const auto migrated = restored.snapshot();
    const auto* migratedMetro = restored.parameters.getRawParameterValue("metronome");
    const auto& actualGlobal = migrated.session.global;
    const auto& expectedGlobal = expected.session.global;
    const bool preserved = migrated.asset && expected.asset &&
        migrated.session.sourcePath == expected.session.sourcePath && migrated.session.sourceHash == expected.session.sourceHash &&
        migrated.asset->sourceHash == expected.asset->sourceHash && migrated.asset->audio.getNumSamples() == expected.asset->audio.getNumSamples() &&
        migrated.selectedPad == expected.selectedPad && samePad(migrated.session.pads[0], expected.session.pads[0]) &&
        samePad(migrated.session.pads[1], expected.session.pads[1]) &&
        std::abs(actualGlobal.targetBpm - expectedGlobal.targetBpm) < .001 &&
        std::abs(actualGlobal.gainDb - expectedGlobal.gainDb) < .001f &&
        std::abs(actualGlobal.semitones - expectedGlobal.semitones) < .001f && actualGlobal.sync == expectedGlobal.sync &&
        migrated.session.beatAnchors.size() == 2 && std::abs(migrated.session.beatAnchors[1].beat - 2) < .00001;
    check(wasOn && migratedMetro && migratedMetro->load() < .5f && !actualGlobal.metronome && preserved,
          "legacy state turns METRO off while preserving nondefault controls, source, pads and beat grid");

    // Both corruptions have 410 old parameters before migration. The first
    // duplicates an existing ID; the second has an out-of-range value. Each
    // also changes pad metadata and selection so an accidental restore is visible.
    auto rejectCorrupt = [&](bool duplicate) {
        juce::XmlElement corrupt(*legacy);
        auto* children = corrupt.getChildByName("PARAMETERS");
        auto* first = children->getChildElement(0); auto* second = children->getChildElement(1);
        if (!first || !second) return false;
        if (duplicate) second->setAttribute("id", first->getStringAttribute("id"));
        else first->setAttribute("value", "999999999");
        corrupt.setAttribute("selectedPad", (migrated.selectedPad + 1) % padCount);
        if (auto* pads = corrupt.getChildByName("PADS"))
            if (auto* pad = pads->getChildElement(0)) pad->setAttribute("name", "CORRUPT");
        juce::MemoryBlock invalid; juce::AudioProcessor::copyXmlToBinary(corrupt, invalid);
        restored.setStateInformation(invalid.getData(), (int)invalid.getSize());
        const auto after = restored.snapshot();
        return after.asset == migrated.asset && after.selectedPad == migrated.selectedPad &&
               after.session.sourcePath == migrated.session.sourcePath && after.session.sourceHash == migrated.session.sourceHash &&
               samePad(after.session.pads[0], migrated.session.pads[0]) && samePad(after.session.pads[1], migrated.session.pads[1]) &&
               std::abs(after.session.global.targetBpm - actualGlobal.targetBpm) < .001 &&
               std::abs(after.session.global.gainDb - actualGlobal.gainDb) < .001f && !after.session.global.metronome;
    };
    check(rejectCorrupt(true) && rejectCorrupt(false),
          "duplicate or out-of-range legacy parameters are rejected without clobbering the current session");
}
void processorTests() {
    const auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("slicelab-markers-" + juce::Uuid().toString());
    dir.createDirectory();
    const auto wav = dir.getChildFile("source.wav"), csv = dir.getChildFile("markers.csv");
    juce::AudioBuffer<float> audio(2, 2 * 44100); audio.clear();
    juce::WavAudioFormat format; std::unique_ptr<juce::OutputStream> stream(wav.createOutputStream());
    auto writer = format.createWriterFor(stream, juce::AudioFormatWriterOptions().withSampleRate(44100).withNumChannels(2).withBitsPerSample(24));
    const bool wrote = writer && writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples()); writer.reset();
    SliceLabAudioProcessor processor;
    processor.loadSample(wav);
    for (int i = 0; i < 1000 && processor.snapshot().busy; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const bool loaded = wrote && processor.snapshot().asset != nullptr;
    check(loaded, "load a source for atomic processor import");
    if (!loaded) { dir.deleteRecursively(); return; }
    auto favorite = processor.snapshot().session.pads[1]; favorite.favorite = true; favorite.startSeconds = .7; favorite.endSeconds = .9; favorite.name = "Protected";
    processor.setPad(1, favorite);
    const auto before = processor.snapshot();
    csv.replaceWithText("seconds,name\n1.25,Later\n0.25,\"Intro, high\"\n");
    auto result = processor.importMarkers(csv); const auto imported = processor.snapshot();
    check(result.wasOk() && imported.asset == before.asset && samePad(imported.session.pads[1], favorite) &&
          imported.session.pads[0].startSeconds == .25 && imported.session.pads[0].endSeconds == 1.25 && imported.session.pads[0].name == "Intro, high" &&
          imported.session.pads[2].startSeconds == 1.25 && imported.session.pads[2].endSeconds == 2 && imported.session.pads[2].name == "Later",
          "import fills unfavorited pads with sorted next-marker boundaries and retains the asset");
    processor.undo(); const auto undone = processor.snapshot();
    check(samePad(undone.session.pads[0], before.session.pads[0]) && samePad(undone.session.pads[1], favorite) && samePad(undone.session.pads[2], before.session.pads[2]),
          "one undo restores all imported pads");
    processor.redo(); const auto redone = processor.snapshot();
    check(samePad(redone.session.pads[0], imported.session.pads[0]) && samePad(redone.session.pads[2], imported.session.pads[2]),
          "one redo restores the complete import");
    csv.replaceWithText("0.5,First\n0.5,Duplicate\n"); result = processor.importMarkers(csv);
    const auto rejected = processor.snapshot();
    check(result.failed() && samePad(rejected.session.pads[0], redone.session.pads[0]) && samePad(rejected.session.pads[2], redone.session.pads[2]),
          "invalid marker file leaves current pads unchanged");
    const char hiddenTail[] = {'0', ',', 'a', '\0', '1', ',', 'b'};
    csv.replaceWithData(hiddenTail, sizeof(hiddenTail)); const auto nulResult = processor.importMarkers(csv);
    const char invalidUtf8[] = {'0', ',', (char)0xc0, (char)0x80};
    csv.replaceWithData(invalidUtf8, sizeof(invalidUtf8)); const auto utf8Result = processor.importMarkers(csv);
    check(nulResult.failed() && utf8Result.failed() && samePad(processor.snapshot().session.pads[0], redone.session.pads[0]),
          "invalid UTF-8 and embedded NUL cannot hide malformed file data");
    for (int i = 0; i < 31; ++i) { auto pad = processor.snapshot().session.pads[(std::size_t)i]; pad.favorite = true; processor.setPad(i, pad); }
    const auto almostFull = processor.snapshot(); csv.replaceWithText("0.2,one\n1.0,two\n"); result = processor.importMarkers(csv);
    const auto refused = processor.snapshot(); bool unchanged = result.failed();
    for (int i = 0; i < padCount; ++i) unchanged &= samePad(almostFull.session.pads[(std::size_t)i], refused.session.pads[(std::size_t)i]);
    check(unchanged, "insufficient unprotected slots rejects the entire import");
    auto global = processor.snapshot().session.global; global.targetBpm = 137; global.gainDb = -9; global.semitones = 5; global.sync = false;
    processor.setGlobal(global);
    auto customPad = processor.snapshot().session.pads[0]; customPad.name = "Legacy fixture"; customPad.gainDb = -11; customPad.semitones = 7; customPad.reverse = true;
    processor.setPad(0, customPad); processor.setBeatAnchors({{0, 0}, {1, 2}}); processor.selectPad(7);
    auto* metro = processor.parameters.getParameter("metronome");
    if (metro) metro->setValueNotifyingHost(1.0f);
    juce::MemoryBlock saved; processor.getStateInformation(saved); SliceLabAudioProcessor restored; restored.setStateInformation(saved.getData(), (int)saved.getSize());
    const auto* restoredMetro = restored.parameters.getRawParameterValue("metronome");
    check(metro && restoredMetro && restoredMetro->load() > .5f && restored.snapshot().session.global.metronome,
          "host metronome parameter survives project state restoration");
    legacyStateTests(saved, processor.snapshot(), restored);
    dir.deleteRecursively();
}
}

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    setenv("SLICELAB_KEY_ANALYZER", "off", 1);
    parserTests(); metronomeTests(); offAndPadTests(); processorTests();
    std::cout << failures << " failure(s)\n";
    return failures == 0 ? 0 : 1;
}
