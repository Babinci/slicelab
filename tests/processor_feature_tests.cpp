#include "PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

using namespace slicelab;
namespace {
int failures=0;
void check(bool condition,const char* message) {
    std::cout << (condition ? "PASS: " : "FAIL: ") << message << '\n';
    if(!condition) ++failures;
}
bool waitUntil(const std::function<bool()>& condition,int milliseconds=15000) {
    for(int elapsed=0;elapsed<milliseconds;elapsed+=10) {
        if(condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return condition();
}
bool load(SliceLabAudioProcessor& processor,const juce::File& file) {
    processor.loadSample(file);
    return waitUntil([&]{return !processor.snapshot().busy;}) && processor.snapshot().asset && processor.snapshot().session.sourcePath==file.getFullPathName();
}
bool writeWav(const juce::File& file,const juce::AudioBuffer<float>& audio) {
    juce::WavAudioFormat format; std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    const auto options=juce::AudioFormatWriterOptions().withSampleRate(44100).withNumChannels(2).withBitsPerSample(24);
    auto writer=format.createWriterFor(stream,options);
    return writer && writer->writeFromAudioSampleBuffer(audio,0,audio.getNumSamples());
}
bool transcode(const juce::File& input,const juce::File& output) {
    juce::ChildProcess process;
    const juce::StringArray args {"ffmpeg","-nostdin","-v","error","-y","-i",input.getFullPathName(),output.getFullPathName()};
    return process.start(args) && process.waitForProcessToFinish(20000) && process.getExitCode()==0;
}
void writeSparseWav(const juce::File& file,std::uint32_t frames) {
    std::ofstream out(file.getFullPathName().toStdString(),std::ios::binary);
    auto put16=[&](std::uint16_t n){for(int i=0;i<2;++i) out.put((char)((n>>(8*i))&255));};
    auto put32=[&](std::uint32_t n){for(int i=0;i<4;++i) out.put((char)((n>>(8*i))&255));};
    const std::uint32_t bytes=frames*4u;
    out.write("RIFF",4); put32(36u+bytes); out.write("WAVEfmt ",8); put32(16); put16(1); put16(2); put32(44100); put32(176400); put16(4); put16(16);
    out.write("data",4); put32(bytes); out.seekp((std::streamoff)44+bytes-1); out.put(0);
}
std::vector<float> render(SliceLabAudioProcessor& processor) {
    constexpr int rate=44100, block=256, frames=rate;
    processor.prepareToPlay(rate,block);
    juce::AudioBuffer<float> audio(2,block); juce::MidiBuffer midi; std::vector<float> result;
    for(int pos=0;pos<frames;pos+=block) {
        midi.clear(); if(pos==0) midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)127),0);
        processor.processBlock(audio,midi);
        result.insert(result.end(),audio.getReadPointer(0),audio.getReadPointer(0)+block);
    }
    return result;
}
double frequency(const std::vector<float>& audio) {
    const int first=4410,last=22050; int crossings=0;
    for(int i=first+1;i<last;++i) if(audio[(size_t)i-1]<=0 && audio[(size_t)i]>0) ++crossings;
    return crossings*44100.0/(last-first);
}
void behavior(const juce::File& dir) {
    const auto wav=dir.getChildFile(juce::String::fromUTF8("śpiew 日本語 source.wav"));
    juce::AudioBuffer<float> audio(2,44100*6); audio.clear();
    for(int i=0;i<audio.getNumSamples();++i) {
        const double t=i/44100.0;
        float value=(float)(.035*std::sin(2*juce::MathConstants<double>::pi*220*t));
        for(const auto& hit:std::array<std::pair<double,double>,6>{{{.5,.25},{1.3,.4},{2.0,.2},{3.1,.95},{4.0,.35},{4.9,.5}}}) {
            const double dt=t-hit.first;
            if(dt>=0 && dt<.06) value+=(float)(hit.second*std::exp(-dt*35)*std::sin(2*juce::MathConstants<double>::pi*730*dt));
        }
        for(int c=0;c<2;++c) audio.setSample(c,i,value);
    }
    check(writeWav(wav,audio),"write the Unicode waveform fixture");
    SliceLabAudioProcessor processor;
    check(load(processor,wav),"decode WAV at a Unicode path");
    for(const char* extension:{"aiff","flac","ogg","mp3"}) {
        const auto output=wav.withFileExtension(extension);
        const auto label=juce::String("decode generated ")+extension+" at a Unicode path";
        const bool generated=transcode(wav,output);
        check(generated && load(processor,output) && processor.snapshot().asset->audio.getMagnitude(0,0,44100)>.01f,label.toRawUTF8());
    }
    check(load(processor,wav),"reload the waveform for slicing");
    auto pad=processor.snapshot().session.pads[31]; pad.startSeconds=.4; pad.endSeconds=.7; pad.favorite=true; processor.setPad(31,pad);
    processor.autoSlice(SliceMode::Transients,5,.5f); const auto transient=processor.snapshot().session;
    processor.autoSlice(SliceMode::FindBest,5,.5f); const auto suggested=processor.snapshot().session;
    bool different=false, strong=false;
    for(int i=0;i<5;++i) {
        different |= std::abs(transient.pads[(size_t)i].startSeconds-suggested.pads[(size_t)i].startSeconds)>.01;
        strong |= std::abs(suggested.pads[(size_t)i].startSeconds-3.1)<.09;
    }
    check(different && strong,"Suggest selects a prominent audio event and differs from transient slicing");
    check(suggested.pads[31].favorite && std::abs(suggested.pads[31].startSeconds-.4)<.001,"Suggest preserves a favorite pad");
    processor.undo(); check(std::abs(processor.snapshot().session.pads[1].startSeconds-transient.pads[1].startSeconds)<.0001,"undo restores the previous slices");
    processor.redo(); check(std::abs(processor.snapshot().session.pads[1].startSeconds-suggested.pads[1].startSeconds)<.0001,"redo restores the suggestions");
    processor.autoSlice(SliceMode::FindBest,5,.5f); const auto again=processor.snapshot().session;
    bool same=true; for(int i=0;i<5;++i) same &= std::abs(again.pads[(size_t)i].startSeconds-suggested.pads[(size_t)i].startSeconds)<.000001;
    check(same,"Suggest is deterministic");

    auto global=processor.snapshot().session.global; global.sourceBpm=120; global.gridDivisionBeats=.5; processor.setGlobal(global);
    processor.setBeatAnchors({{0,0},{1,2},{2,6}}); processor.autoSlice(SliceMode::Grid,12,.5f);
    const auto grid=processor.snapshot();
    check(std::abs(grid.session.pads[1].startSeconds-.25)<.0001 && std::abs(grid.session.pads[5].startSeconds-1.125)<.0001,"fractional grid divisions follow piecewise beat anchors");
    check(grid.session.pads[31].favorite && std::abs(grid.session.pads[31].startSeconds-.4)<.001,"grid slicing preserves favorites");
    juce::MemoryBlock data; processor.getStateInformation(data); SliceLabAudioProcessor restored; restored.setStateInformation(data.getData(),(int)data.getSize());
    check(std::abs(restored.snapshot().session.global.gridDivisionBeats-.5)<1e-9,"grid interval survives project state restoration");
    global=processor.snapshot().session.global; global.gridDivisionBeats=-1; processor.setGlobal(global);
    check(std::abs(processor.snapshot().session.global.gridDivisionBeats-.0625)<1e-9,"grid interval clamps to the lower bound");
    global=processor.snapshot().session.global; global.gridDivisionBeats=9; processor.setGlobal(global);
    check(std::abs(processor.snapshot().session.global.gridDivisionBeats-4)<1e-9,"grid interval clamps to the upper bound");

    auto before=processor.snapshot().asset;
    for(int i=0;i<20;++i) { auto edit=processor.snapshot().session.pads[0]; edit.gainDb=-.1f*i; processor.setPad(0,edit); }
    check(processor.snapshot().asset==before,"pad edits share the decoded source asset");
    const auto huge=dir.getChildFile("oversize.wav"); writeSparseWav(huge,128u*1024u*1024u/8u+1u);
    processor.loadSample(huge); waitUntil([&]{return !processor.snapshot().busy;});
    check(processor.snapshot().asset==before && processor.snapshot().error.containsIgnoreCase("limit"),"oversize source is rejected without replacing the session");
    juce::MemoryBlock current; processor.getStateInformation(current);
    auto invalid=juce::AudioProcessor::getXmlFromBinary(current.getData(),(int)current.getSize());
    invalid->setAttribute("source",huge.getFullPathName()); juce::MemoryBlock invalidState;
    juce::AudioProcessor::copyXmlToBinary(*invalid,invalidState);
    const auto priorPath=processor.snapshot().session.sourcePath;
    processor.setStateInformation(invalidState.getData(),(int)invalidState.getSize());
    check(processor.snapshot().asset==before && processor.snapshot().session.sourcePath==priorPath && processor.snapshot().error.containsIgnoreCase("limit"),"oversize preset source leaves the current session playable");
}
void targetKey(const juce::File& dir) {
    const auto silent=dir.getChildFile("silent.wav"), tone=dir.getChildFile("tone.wav");
    juce::AudioBuffer<float> audio(2,44100*2); audio.clear(); writeWav(silent,audio);
    for(int i=0;i<audio.getNumSamples();++i) for(int c=0;c<2;++c) audio.setSample(c,i,(float)(.4*std::sin(2*juce::MathConstants<double>::pi*220*i/44100)));
    writeWav(tone,audio); SliceLabAudioProcessor processor;
    check(load(processor,silent) && !processor.setTargetKey(0),"unknown source key cannot select a target tonic");
    check(load(processor,tone),"load the pitch fixture");
    auto global=processor.snapshot().session.global; global.keyOverride=21; global.sync=false; global.sourceBpm=120; global.targetBpm=120; processor.setGlobal(global);
    auto pad=processor.snapshot().session.pads[0]; pad.startSeconds=0; pad.endSeconds=1; pad.attackMs=0; processor.setPad(0,pad);
    check(processor.setTargetKey(0),"select a target tonic with a manual source key");
    const auto state=processor.snapshot().session.global;
    check(state.keyOverride==21 && std::abs(state.semitones-3)<.01 && std::abs(processor.parameters.getRawParameterValue("global_semitones")->load()-3)<.01,"target key sets the nearest host-automatable semitone interval and preserves source mode");
    const auto output=render(processor); const double heard=frequency(output);
    check(std::abs(heard-261.63)<20,"target tonic changes the rendered pitch");
    processor.undo(); check(std::abs(processor.snapshot().session.global.semitones)<.01,"target-key change is undoable");
    check(load(processor,silent) && processor.snapshot().session.global.keyOverride==-1 && !processor.setTargetKey(0),"replacing the source clears its manual key correction");
}
void analyzerBoundary(const juce::File& dir) {
    const auto script=dir.getChildFile("fake_key.py"), source=dir.getChildFile("tone.wav"), low=dir.getChildFile("low.wav");
    script.replaceWithText("import sys,time\ntime.sleep(.1)\nprint('KEY_RESULT\\tkey=F major\\tcamelot=7B\\tconfidence=' + ('low' if 'low.wav' in sys.argv[1] else 'high'))\n");
    source.copyFileTo(low);
    setenv("SLICELAB_KEY_ANALYZER",script.getFullPathName().toRawUTF8(),1); setenv("SLICELAB_KEY_PYTHON","/usr/bin/python3",1);
    SliceLabAudioProcessor processor; check(load(processor,source),"load source for external key analysis");
    check(waitUntil([&]{auto a=processor.snapshot().asset; return a && a->keyConfidenceLabel=="S-KEY high";}) && processor.snapshot().asset->detectedKey=="F major","background analyzer supplies its key and confidence grade");
    check(load(processor,low),"load source with ambiguous analyzer result");
    check(waitUntil([&]{auto a=processor.snapshot().asset; return a && a->keyConfidenceLabel=="S-KEY low";}) && processor.snapshot().asset->detectedKey=="Unknown","low external confidence leaves the key unknown");
    processor.loadSample(source); processor.loadSample(low);
    check(waitUntil([&]{return !processor.snapshot().busy;}) && processor.snapshot().session.sourcePath==low.getFullPathName() &&
          waitUntil([&]{auto a=processor.snapshot().asset; return a && a->keyConfidenceLabel=="S-KEY low";}),"rapid source replacements discard stale analysis and loads");
    setenv("SLICELAB_KEY_ANALYZER","off",1); unsetenv("SLICELAB_KEY_PYTHON");
}
void memoryBudget(const juce::File& dir) {
    const auto large=dir.getChildFile("memory.wav");
    writeSparseWav(large,256u*1024u*1024u/(5u*8u)+2048u);
    SliceLabAudioProcessor processor;
    check(load(processor,large),"load a source below the decoded-source limit");
    juce::MemoryBlock state; processor.getStateInformation(state);
    auto xml=juce::AudioProcessor::getXmlFromBinary(state.getData(),(int)state.getSize());
    xml->setAttribute("stemManifest",dir.getChildFile("large-stems.json").getFullPathName());
    juce::MemoryBlock invalidState; juce::AudioProcessor::copyXmlToBinary(*xml,invalidState);
    auto current=processor.snapshot().asset; processor.setStateInformation(invalidState.getData(),(int)invalidState.getSize());
    check(processor.snapshot().asset==current && processor.snapshot().error.containsIgnoreCase("too large"),"oversize preset stem set leaves the current session playable");
    current.reset();
    processor.separateStems(); check(processor.snapshot().error.containsIgnoreCase("too large"),"reject a four-stem set above the decoded-asset limit");
    std::weak_ptr<const SampleAsset> oldest=processor.snapshot().asset;
    bool loaded=true; for(int i=0;i<5;++i) loaded &= load(processor,large);
    check(loaded && waitUntil([&]{return oldest.expired();},4000),"unique prior assets are evicted when undo PCM exceeds its budget");
}
void realAnalyzer() {
    const auto enabled=juce::SystemStats::getEnvironmentVariable("SLICELAB_VERIFY_EXTERNAL_KEY",{});
    if(enabled!="1") return;
    unsetenv("SLICELAB_KEY_ANALYZER"); unsetenv("SLICELAB_KEY_PYTHON");
    const auto source=juce::File(juce::String(SLICELAB_TEST_ROOT)+"/sample_test.wav"); SliceLabAudioProcessor processor;
    const bool loaded=load(processor,source);
    const bool analyzed=loaded && waitUntil([&]{auto a=processor.snapshot().asset; return a && a->keyConfidenceLabel.isNotEmpty();},50000);
    check(analyzed && processor.snapshot().asset->detectedKey=="F major" && processor.snapshot().asset->keyConfidenceLabel=="S-KEY high","installed S-KEY and Essentia analyze the supplied sample");
}
}
int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    setenv("SLICELAB_KEY_ANALYZER","off",1);
    const auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("slicelab-features-"+juce::Uuid().toString()); dir.createDirectory();
    behavior(dir); targetKey(dir); analyzerBoundary(dir);
    if(juce::SystemStats::getEnvironmentVariable("SLICELAB_VERIFY_MEMORY",{})=="1") memoryBudget(dir);
    realAnalyzer(); dir.deleteRecursively();
    std::cout << failures << " failure(s)\n";
    return failures==0 ? 0 : 1;
}
