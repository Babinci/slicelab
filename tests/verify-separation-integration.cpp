#include "PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_cryptography/juce_cryptography.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

namespace {
int failures=0;
void check(bool value,const char* what) {
    std::cout << (value ? "PASS: " : "FAIL: ") << what << std::endl;
    if(!value) ++failures;
}
bool waitUntil(const std::function<bool()>& predicate,int timeoutMs=30000) {
    for(int n=0;n<timeoutMs;n+=20) {
        if(predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return predicate();
}
bool load(SliceLabAudioProcessor& processor,const juce::File& path) {
    processor.loadSample(path);
    return waitUntil([&]{return !processor.snapshot().busy;}) && processor.snapshot().asset!=nullptr;
}
bool writeExpectedPcm(const slicelab::SampleAsset& asset,const juce::File& file) {
    std::ofstream out(file.getFullPathName().toStdString(),std::ios::binary);
    for(int i=0;i<asset.audio.getNumSamples();++i) for(int c=0;c<2;++c) {
        const auto sample=asset.audio.getSample(c,i);
        out.write(reinterpret_cast<const char*>(&sample),sizeof(sample));
    }
    return !!out;
}
void env(const char* key,const juce::String& value) { setenv(key,value.toRawUTF8(),1); }

void fakeCases(const juce::File& mp3,const juce::File& wav,const juce::File& fake,const juce::File& root) {
    env("SLICELAB_STEM_WORKER",fake.getFullPathName());
    const auto highAmplitude=root.getChildFile("high-amplitude-mono.aiff");
    juce::ChildProcess ffmpeg;
    const juce::StringArray ffmpegArgs {"ffmpeg","-nostdin","-v","error","-y","-i",wav.getFullPathName(),
                                        "-af","volume=6","-ac","1","-c:a","pcm_f32be",highAmplitude.getFullPathName()};
    const bool generated=ffmpeg.start(ffmpegArgs) && ffmpeg.waitForProcessToFinish(30000) && ffmpeg.getExitCode()==0;
    check(generated,"generate high-amplitude mono AIFF fixture");
    const auto fourChannel=root.getChildFile("four-channel.wav");
    juce::ChildProcess fourChannelFfmpeg;
    const juce::StringArray fourChannelArgs {"ffmpeg","-nostdin","-v","error","-y","-i",wav.getFullPathName(),
        "-af","pan=quad|c0=c0|c1=c1|c2=0.25*c0|c3=-0.5*c1","-c:a","pcm_s16le",fourChannel.getFullPathName()};
    check(fourChannelFfmpeg.start(fourChannelArgs) && fourChannelFfmpeg.waitForProcessToFinish(30000) && fourChannelFfmpeg.getExitCode()==0,
          "generate distinct four-channel WAV fixture");
    for(const juce::String mode:{"normal","high-amplitude-mono","four-channel-wav","expected-original","bad-transport-hash","bad-stem-frames","modify-source","changed-before","wait"}) {
        const auto dir=root.getChildFile(mode); dir.createDirectory();
        dir.getChildFile("called.json").deleteFile();
        const auto original=dir.getChildFile(juce::String::fromUTF8(mode=="expected-original" || mode=="four-channel-wav" ? "śpiew 日本語.wav" : mode=="high-amplitude-mono" ? "śpiew 日本語.aiff" : "śpiew 日本語.mp3"));
        const auto fixture=mode=="expected-original" ? wav : mode=="high-amplitude-mono" ? highAmplitude : mode=="four-channel-wav" ? fourChannel : mp3;
        check(fixture.copyFileTo(original),"copy synthetic fixture");
        env("SLICELAB_FAKE_DIR",dir.getFullPathName()); env("SLICELAB_FAKE_MODE",mode);
        env("SLICELAB_FAKE_ORIGINAL",original.getFullPathName());
        env("SLICELAB_FAKE_EXPECTED_PCM",dir.getChildFile("expected.f32").getFullPathName());
        SliceLabAudioProcessor processor;
        if(!load(processor,original)) { check(false,"load synthetic input"); continue; }
        const auto prior=processor.snapshot().asset;
        if(mode=="high-amplitude-mono") check(prior->audio.getMagnitude(0,0,prior->audio.getNumSamples())>1.0f &&
            std::memcmp(prior->audio.getReadPointer(0),prior->audio.getReadPointer(1),(size_t)prior->audio.getNumSamples()*sizeof(float))==0,
            "mono fixture contains over-unity audio and loads identically in both channels");
        if(mode=="four-channel-wav") {
            juce::WavAudioFormat format; auto input=original.createInputStream();
            std::unique_ptr<juce::AudioFormatReader> reader(input ? format.createReaderFor(input.release(),true) : nullptr);
            const int count=prior->audio.getNumSamples(); juce::AudioBuffer<float> channels(4,count);
            const bool valid=reader && reader->numChannels==4 && reader->lengthInSamples==count &&
                             reader->read(&channels,0,count,0,true,true);
            const size_t bytes=(size_t)count*sizeof(float);
            check(valid && std::memcmp(channels.getReadPointer(0),prior->audio.getReadPointer(0),bytes)==0 &&
                  std::memcmp(channels.getReadPointer(1),prior->audio.getReadPointer(1),bytes)==0 &&
                  std::memcmp(channels.getReadPointer(0),channels.getReadPointer(2),bytes)!=0 &&
                  std::memcmp(channels.getReadPointer(1),channels.getReadPointer(3),bytes)!=0,
                  "four-channel WAV loads its first two distinct channels exactly");
        }
        check(writeExpectedPcm(*prior,dir.getChildFile("expected.f32")),"write expected decoded PCM");
        if(mode=="changed-before") { std::ofstream out(original.getFullPathName().toStdString(),std::ios::app|std::ios::binary); out << "changed"; }
        processor.separateStems();
        if(mode=="wait") {
            const bool started=waitUntil([&]{return dir.getChildFile("called.json").existsAsFile();});
            check(started,"delayed worker starts");
            processor.loadSample(wav);
            check(waitUntil([&]{return !processor.snapshot().busy;}),"new source finishes loading after cancelling old worker");
            const auto shot=processor.snapshot();
            check(shot.asset && shot.asset->sourcePath==wav.getFullPathName() && !shot.asset->stemsReady,"stale separation cannot replace new source");
            continue;
        }
        check(waitUntil([&]{return !processor.snapshot().busy;}),"separation request completes");
        const auto shot=processor.snapshot();
        if(mode=="normal" || mode=="high-amplitude-mono" || mode=="four-channel-wav") {
            check(shot.asset && shot.asset->stemsReady,"canonical source produces four accepted stems");
            if(shot.asset && shot.asset->stemsReady) {
                const auto manifest=juce::JSON::parse(juce::File(shot.asset->stemManifestPath));
                const auto* obj=manifest.getDynamicObject();
                const auto called=juce::JSON::parse(dir.getChildFile("called.json"));
                const auto* worker=called.getDynamicObject();
                check(obj && worker && obj->getProperty("input_sha256").toString()==prior->sourceHash &&
                      obj->getProperty("transport_input_sha256").toString()==worker->getProperty("sha256").toString() &&
                      obj->getProperty("source_path").toString()==prior->sourcePath &&
                      obj->getProperty("source_decoding").toString().isNotEmpty(),"bound manifest preserves original and transport identities");
                check(worker && shot.asset->audio.getNumSamples()==(int)worker->getProperty("frames") &&
                      (int)worker->getProperty("encoding")==3 && (int)worker->getProperty("bits")==32,
                      "fake worker received exact float32 JUCE timeline");
                if(mode=="four-channel-wav") check(worker && obj && worker->getProperty("input").toString()!=original.getFullPathName() &&
                    obj->getProperty("input_sha256").toString()==juce::SHA256(original).toHexString() &&
                    obj->getProperty("source_path").toString()==original.getFullPathName(),
                    "four-channel WAV uses canonical stereo input and keeps original file identity");
            }
        } else if(mode=="expected-original") {
            const auto marker=juce::JSON::parse(dir.getChildFile("called.json")); const auto* obj=marker.getDynamicObject();
            check(shot.asset && shot.asset->stemsReady && obj && obj->getProperty("input").toString()==original.getFullPathName() &&
                  obj->getProperty("sha256").toString()==prior->sourceHash,"WAV uses original path and checksum");
        } else {
            check(shot.asset==prior && !shot.asset->stemsReady && shot.error.isNotEmpty(),"bad result or changed source leaves original asset intact");
            if(mode=="bad-transport-hash") check(shot.error.containsIgnoreCase("separation input"),"transport hash mismatch is rejected");
            if(mode=="bad-stem-frames") check(shot.error.containsIgnoreCase("misaligned"),"stem frame mismatch is rejected");
            if(mode=="modify-source") check(shot.error.containsIgnoreCase("changed during"),"source mutation during worker is rejected");
            if(mode=="changed-before") check(shot.error.containsIgnoreCase("changed before") && !dir.getChildFile("called.json").existsAsFile(),"source mutation before worker prevents launch");
        }
        if(shot.error.isNotEmpty()) std::cout << "  detail: " << shot.error << std::endl;
    }
}

void realCase(const juce::File& mp3,const juce::File& worker,const juce::File& root) {
    const auto dir=root.getChildFile("real-mp3"); dir.createDirectory();
    env("SLICELAB_STEM_WORKER",worker.getFullPathName()); env("SLICELAB_STEM_CACHE",dir.getChildFile("cache").getFullPathName());
    SliceLabAudioProcessor processor;
    if(!load(processor,mp3)) { check(false,"load real-worker synthetic MP3"); return; }
    const auto prior=processor.snapshot().asset;
    processor.separateStems();
    check(waitUntil([&]{return !processor.snapshot().busy;},180000),"real remote MP3 separation completes");
    const auto shot=processor.snapshot();
    check(shot.asset && shot.asset->stemsReady && shot.asset->sourceHash==prior->sourceHash &&
          shot.asset->audio.getNumSamples()==prior->audio.getNumSamples(),"real remote stems load on JUCE MP3 timeline");
    if(shot.error.isNotEmpty()) std::cout << "  detail: " << shot.error << std::endl;
    if(shot.asset && shot.asset->stemsReady) std::cout << "  manifest: " << shot.asset->stemManifestPath << std::endl;
}
}

int main(int argc,char** argv) {
    if(argc!=5 && argc!=6) {
        std::cerr << "Usage: test MP3 WAV FAKE_WORKER OUTPUT_DIR [REAL_WORKER]" << std::endl; return 2;
    }
    juce::ScopedJuceInitialiser_GUI gui;
    env("SLICELAB_KEY_ANALYZER","off");
    const juce::File mp3(juce::String::fromUTF8(argv[1])),wav(juce::String::fromUTF8(argv[2]));
    const juce::File fake(juce::String::fromUTF8(argv[3])),root(juce::String::fromUTF8(argv[4]));
    root.createDirectory();
    fakeCases(mp3,wav,fake,root);
    if(argc==6) realCase(mp3,juce::File(juce::String::fromUTF8(argv[5])),root);
    std::cout << failures << " failure(s)" << std::endl;
    return failures==0 ? 0 : 1;
}
