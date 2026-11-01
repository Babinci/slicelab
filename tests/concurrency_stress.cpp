#include "SamplerEngine.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

using namespace slicelab;
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--tsan-positive-control") == 0) {
        // Deliberately racy only in this opt-in diagnostic mode: demonstrate
        // that TSan reports a payload race before trusting a clean stress run.
        volatile int payload = 0;
        std::atomic<bool> go { false };
        std::thread writer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            payload = 1;
        });
        go.store(true, std::memory_order_release);
        payload = 2;
        writer.join();
        std::cout << "Positive-control payload=" << payload << '\n';
        return 0;
    }
    SamplerEngine engine; engine.prepareScheduled(48000,128,960);
    SessionState state;
    for(auto& pad:state.pads) { pad.startSeconds=0; pad.endSeconds=.25; }
    std::atomic<bool> running {true};
    std::atomic<unsigned> callbacks {0}, invalid {0};
    std::thread audio([&] {
        PlaybackParameters parameters; parameters.sync=false; parameters.sourceBpm=parameters.targetBpm=120; parameters.mono=false;
        for(auto& pad:parameters.pads) { pad.loop=true; pad.attackMs=0; }
        std::array<int,padCount> offsets {}; juce::AudioBuffer<float> buffer(2,128); juce::MidiBuffer midi;
        midi.ensureSize(256);
        while(running.load(std::memory_order_acquire)) {
            midi.clear();
            if(callbacks.load(std::memory_order_relaxed)%29==0)
                midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)100),17);
            engine.processScheduled(buffer,midi,parameters,offsets,true);
            for(int channel=0;channel<2;++channel) for(int i=0;i<128;++i)
                if(!std::isfinite(buffer.getSample(channel,i))) invalid.fetch_add(1,std::memory_order_relaxed);
            callbacks.fetch_add(1,std::memory_order_relaxed);
        }
    });
    std::vector<std::weak_ptr<const SampleAsset>> retired;
    for(int generation=0;generation<500;++generation) {
        auto asset=std::make_shared<SampleAsset>(); asset->sampleRate=48000; asset->generation=(std::uint64_t)generation;
        asset->audio.setSize(2,12000);
        for(int i=0;i<12000;++i) for(int channel=0;channel<2;++channel)
            asset->audio.setSample(channel,i,.1f*std::sin((float)(i*.035+generation)));
        retired.push_back(asset);
        engine.publish(asset,state);
        if(generation%17==0) engine.enqueuePreview(.01);
        if(generation%23==0) engine.enqueueStopPreview();
        engine.collectRetired();
        if(generation%5==0) std::this_thread::yield();
    }
    running.store(false,std::memory_order_release); audio.join();
    engine.release(); engine.publish({},SessionState{}); engine.collectRetired();
    unsigned retained=0;
    for(const auto& asset:retired) if(!asset.expired()) ++retained;
    std::cout<<"Concurrent publication callbacks="<<callbacks.load()<<" invalid_samples="<<invalid.load()<<" unreclaimed_assets="<<retained<<'\n';
    return callbacks.load()>0 && invalid.load()==0 && retained==0 ? 0 : 1;
}
