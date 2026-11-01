#include "PluginProcessor.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <new>
#include <thread>

extern "C" void* __libc_malloc(size_t);
extern "C" void* __libc_calloc(size_t,size_t);
extern "C" void* __libc_realloc(void*,size_t);
extern "C" void* __libc_memalign(size_t,size_t);
extern "C" void __libc_free(void*);

namespace {
thread_local bool watching=false;
thread_local unsigned allocations=0, frees=0;
void allocated() { if(watching) ++allocations; }
void freed(void* p) { if(watching && p) ++frees; }
}
extern "C" void* malloc(size_t n) { allocated(); return __libc_malloc(n); }
extern "C" void* calloc(size_t n,size_t s) { allocated(); return __libc_calloc(n,s); }
extern "C" void* realloc(void* p,size_t n) { allocated(); return __libc_realloc(p,n); }
extern "C" void free(void* p) { freed(p); __libc_free(p); }
extern "C" void* aligned_alloc(size_t a,size_t n) { allocated(); return __libc_memalign(a,n); }
extern "C" int posix_memalign(void** p,size_t a,size_t n) { allocated(); auto* result=__libc_memalign(a,n); if(!result) return ENOMEM; *p=result; return 0; }
void* operator new(size_t n) { allocated(); if(auto* p=__libc_malloc(std::max(size_t(1),n))) return p; throw std::bad_alloc(); }
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { freed(p); __libc_free(p); }
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p,size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,size_t) noexcept { ::operator delete(p); }
void* operator new(size_t n,std::align_val_t a) { allocated(); if(auto* p=__libc_memalign((size_t)a,std::max(size_t(1),n))) return p; throw std::bad_alloc(); }
void* operator new[](size_t n,std::align_val_t a) { return ::operator new(n,a); }
void operator delete(void* p,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete(void* p,size_t,std::align_val_t) noexcept { ::operator delete(p); }
void operator delete[](void* p,size_t,std::align_val_t) noexcept { ::operator delete(p); }

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    setenv("SLICELAB_KEY_ANALYZER","off",1);
    watching=true;
    void* positive=malloc(17); void* aligned=nullptr;
    const int alignedResult=posix_memalign(&aligned,32,64);
    free(positive); free(aligned);
    auto* cpp=::operator new(31); ::operator delete(cpp);
    watching=false;
    if(alignedResult!=0 || allocations!=3 || frees!=3) { std::cerr<<"Allocator positive control failed\n"; return 1; }
    const auto source=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("slicelab-rt-"+juce::Uuid().toString()+".wav");
    juce::AudioBuffer<float> pcm(2,48000);
    for(int i=0;i<pcm.getNumSamples();++i) for(int c=0;c<2;++c) pcm.setSample(c,i,.3f*std::sin((float)(2*juce::MathConstants<double>::pi*440*i/48000)));
    juce::WavAudioFormat format; std::unique_ptr<juce::OutputStream> output(source.createOutputStream());
    auto writer=format.createWriterFor(output,juce::AudioFormatWriterOptions().withSampleRate(48000).withNumChannels(2).withBitsPerSample(24));
    if(!writer || !writer->writeFromAudioSampleBuffer(pcm,0,pcm.getNumSamples())) return 1;
    writer.reset();
    SliceLabAudioProcessor processor; processor.loadSample(source);
    for(int i=0;i<2000 && processor.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if(!processor.snapshot().asset) return 1;
    processor.autoSlice(slicelab::SliceMode::Equal,32,.5f);
    unsigned failures=0;
    for(double rate:{44100.0,48000.0,96000.0}) {
        processor.prepareToPlay(rate,512); processor.setNonRealtime(false);
        juce::AudioBuffer<float> audio(2,512); juce::MidiBuffer midi; midi.ensureSize(16384);
        auto mono=processor.snapshot().session.global; mono.sync=false; mono.mono=true; mono.semitones=7; mono.repitch=false; mono.sourceBpm=mono.targetBpm=120; mono.stemEnabled.fill(true);
        processor.setGlobal(mono);
        auto longPad=processor.snapshot().session.pads[0]; longPad.startSeconds=0; longPad.endSeconds=1; longPad.loop=true; processor.setPad(0,longPad);
        float audiblePeak=0;
        for(int block=0;block<64;++block) {
            midi.clear(); if(block==0) midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)100),37);
            allocations=frees=0; watching=true; processor.processBlock(audio,midi); watching=false;
            if(allocations || frees) ++failures;
            audiblePeak=std::max(audiblePeak,audio.getMagnitude(0,0,512));
        }
        if(audiblePeak<.01f || processor.snapshot().error.isNotEmpty()) { ++failures; std::cerr<<"Single-voice R3 allocation gate was silent or overloaded at "<<rate<<'\n'; }
        processor.stopAll();
        auto settings=processor.snapshot().session.global; settings.sync=false; settings.mono=false;
        for(int block=0;block<240;++block) {
            midi.clear();
            if(block%40==0) {
                settings.semitones=(float)((block/40)%3*12-12); settings.targetBpm=block%80 ? 240 : 60;
                settings.repitch=block>=160; settings.stemEnabled[(size_t)(block/40%4)]=!settings.stemEnabled[(size_t)(block/40%4)];
                processor.setGlobal(settings); processor.auditionSource(.1);
                for(int note=36;note<68;++note) midi.addEvent(juce::MidiMessage::noteOn(1,note,(juce::uint8)100),(note-36)*7);
            }
            if(block%40==20) {
                processor.stopSourcePreview();
                for(int note=36;note<68;++note) midi.addEvent(juce::MidiMessage::noteOff(1,note),31);
            }
            allocations=frees=0; watching=true;
            processor.processBlock(audio,midi);
            watching=false;
            if(allocations || frees) { ++failures; std::cerr<<"Heap operation at rate="<<rate<<" block="<<block<<" allocations="<<allocations<<" frees="<<frees<<'\n'; }
        }
        processor.releaseResources();
    }
    source.deleteFile();
    std::cout<<"Actual processor realtime callback allocation failures: "<<failures<<'\n';
    return failures==0 ? 0 : 1;
}
