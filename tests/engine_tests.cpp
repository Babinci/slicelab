#include "PluginProcessor.h"
#include "SamplerEngine.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_cryptography/juce_cryptography.h>
#include <cmath>
#include <iostream>
#include <thread>

using namespace slicelab;
namespace {
int failures=0;
void check(bool ok, const char* message) { if(!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; } else std::cout << "PASS: " << message << '\n'; }
std::shared_ptr<SampleAsset> tone(double rate, double seconds, float frequency=440.0f) {
    auto a=std::make_shared<SampleAsset>(); a->sampleRate=rate; int n=(int)(seconds*rate); a->audio.setSize(2,n);
    for(int i=0;i<n;++i) for(int c=0;c<2;++c) a->audio.setSample(c,i,0.5f*std::sin((float)(2.0*juce::MathConstants<double>::pi*frequency*i/rate)));
    return a;
}
struct Render { std::vector<float> left,right; };
Render render(SamplerEngine& engine, const PlaybackParameters& p, double rate, int samples, int note=36, int offset=0, int noteOff=-1, int bus=0) {
    juce::AudioBuffer<float> buffer(bus ? 4 : 2,256); juce::MidiBuffer midi; std::array<int,padCount> offsets {}; if(bus) offsets[(size_t)bus]=2;
    Render out;
    for(int start=0;start<samples;start+=256) {
        const int count=std::min(256,samples-start); buffer.setSize(bus ? 4 : 2,count,false,false,true); midi.clear();
        if(offset>=start && offset<start+count) midi.addEvent(juce::MidiMessage::noteOn(1,note,(juce::uint8)127),offset-start);
        if(noteOff>=start && noteOff<start+count) midi.addEvent(juce::MidiMessage::noteOff(1,note),noteOff-start);
        engine.process(buffer,midi,p,offsets);
        out.left.insert(out.left.end(),buffer.getReadPointer(0),buffer.getReadPointer(0)+count);
        out.right.insert(out.right.end(),buffer.getReadPointer(bus ? 2 : 1),buffer.getReadPointer(bus ? 2 : 1)+count);
    }
    return out;
}
float peak(const std::vector<float>& v,int begin,int end) { float m=0; for(int i=std::max(0,begin);i<std::min((int)v.size(),end);++i) m=std::max(m,std::abs(v[(size_t)i])); return m; }
double freq(const std::vector<float>& v,int begin,int end,double rate) { int crossings=0; for(int i=begin+1;i<std::min(end,(int)v.size());++i) if(v[(size_t)i-1]<=0 && v[(size_t)i]>0) ++crossings; return crossings*rate/(end-begin); }
int lastAudible(const std::vector<float>& v) { for(int i=(int)v.size()-1;i>=0;--i) if(std::abs(v[(size_t)i])>0.01f) return i; return 0; }
SessionState onePad(double seconds) { SessionState s; s.pads[0].startSeconds=0; s.pads[0].endSeconds=seconds; return s; }
PlaybackParameters defaults() { PlaybackParameters p; p.sync=false; p.sourceBpm=120; p.targetBpm=120; p.gainDb=0; p.pads[0].attackMs=0; p.pads[0].releaseMs=10; return p; }

void engineBehavior(double rate) {
    auto asset=tone(44100,0.5); auto state=onePad(0.5); auto p=defaults(); SamplerEngine engine; engine.prepare(rate,256); engine.publish(asset,state);
    auto out=render(engine,p,rate,(int)(rate*0.7),36,63);
    check(peak(out.left,0,63)==0 && peak(out.left,64,300)>0.05f,"MIDI note starts at its sample offset");
    check(std::abs(freq(out.left,(int)(rate*.1),(int)(rate*.3),rate)-440)<15,"source sample rate is converted correctly");
    p.pads[0].semitones=12; engine.publish(asset,state); out=render(engine,p,rate,(int)(rate*.7));
    check(std::abs(freq(out.left,(int)(rate*.1),(int)(rate*.35),rate)-880)<45,"pitch shift changes frequency");
    check(lastAudible(out.left)>rate*.4 && lastAudible(out.left)<rate*.62,"pitch shift preserves duration");
    if(rate==44100) {
        p=defaults(); p.keyboardMode=true; p.rootMidiNote=60; p.selectedPad=0; engine.enqueueNote(72,1,true);
        out=render(engine,p,rate,(int)(rate*.45),36,-1);
        check(std::abs(freq(out.left,(int)(rate*.1),(int)(rate*.35),rate)-880)<45,"computer keyboard notes transpose the selected pad");
    }
    p=defaults(); p.pads[0].semitones=0; p.targetBpm=60; engine.publish(asset,state); out=render(engine,p,rate,(int)(rate*1.2));
    check(std::abs(freq(out.left,(int)(rate*.2),(int)(rate*.7),rate)-440)<35,"tempo stretch preserves pitch");
    check(lastAudible(out.left)>rate*.85 && lastAudible(out.left)<rate*1.15,"free target tempo changes duration");
    p=defaults(); p.hold=true; p.pads[0].loop=true; engine.enqueueStop(); out=render(engine,p,rate,(int)(rate*.7),36,0,(int)(rate*.15));
    check(peak(out.left,(int)(rate*.22),(int)(rate*.35))<0.001f,"hold note-off releases the voice");
    p.hold=false; engine.enqueueStop(); out=render(engine,p,rate,(int)(rate*.7),36,0,(int)(rate*.15));
    check(peak(out.left,(int)(rate*.55),(int)(rate*.65))>0.05f,"trigger mode loops after note-off");
    p=defaults(); p.pads[0].output=1; out=render(engine,p,rate,(int)(rate*.2),36,0,-1,1);
    check(peak(out.left,0,(int)(rate*.2))<0.00001f && peak(out.right,0,(int)(rate*.2))>0.05f,"pad output reaches the selected auxiliary bus");
    p=defaults(); p.stemEnabled={false,false,false,false}; out=render(engine,p,rate,(int)(rate*.2));
    check(peak(out.left,0,(int)(rate*.2))<0.00001f,"all stems off is silent");
}

void continuousAutomation() {
    constexpr double rate=48000;
    auto asset=tone(rate,3); asset->stemsReady=true;
    for(auto& stem:asset->stems) { stem=asset->audio; stem.applyGain(.25f); }
    auto p=defaults(); p.pads[0].loop=true; SamplerEngine engine; engine.prepare(rate,256); engine.publish(asset,onePad(3));
    auto out=render(engine,p,rate,(int)(rate*.15));
    p.stemEnabled={false,false,false,false}; out=render(engine,p,rate,(int)(rate*.15),36,-1);
    check(peak(out.left,(int)(rate*.1),(int)(rate*.14))<.00001f,"stem mute silences an already playing voice");
    p.stemEnabled={true,true,true,true}; p.stemSolo[2]=true; out=render(engine,p,rate,(int)(rate*.15),36,-1);
    check(peak(out.left,(int)(rate*.1),(int)(rate*.14))>.08f && peak(out.left,(int)(rate*.1),(int)(rate*.14))<.16f,"stem solo changes an already playing voice");
    p.targetBpm=60; out=render(engine,p,rate,(int)(rate*.35),36,-1);
    check(std::abs(freq(out.left,(int)(rate*.15),(int)(rate*.3),rate)-440)<35,"live tempo change preserves pitch");
    p.pads[0].semitones=12; out=render(engine,p,rate,(int)(rate*.4),36,-1);
    check(std::abs(freq(out.left,(int)(rate*.2),(int)(rate*.35),rate)-880)<55,"live pitch change reaches an already playing voice");
}

void reverseAndPads() {
    auto asset=std::make_shared<SampleAsset>(); asset->sampleRate=44100; asset->audio.setSize(2,44100);
    for(int i=0;i<44100;++i) for(int c=0;c<2;++c) asset->audio.setSample(c,i,i<22050?0.1f:0.8f);
    SessionState state; for(int i=0;i<padCount;++i) {state.pads[(size_t)i].startSeconds=0;state.pads[(size_t)i].endSeconds=1;}
    auto p=defaults(); p.pads[0].reverse=true; SamplerEngine engine; engine.prepare(44100,256); engine.publish(asset,state);
    auto out=render(engine,p,44100,4410); check(peak(out.left,300,3000)>0.5f,"reverse starts at the slice end");
    p.pads[0].reverse=false;
    p.pads[0].gainDb=-60; engine.enqueueStop(); engine.enqueuePreview(.75);
    out=render(engine,p,44100,2000,36,-1); check(peak(out.left,300,1800)>0.5f,"source preview starts at cursor and bypasses pad gain");
    engine.enqueueStopPreview(); out=render(engine,p,44100,1000,36,-1); check(peak(out.left,0,1000)<0.00001f,"source preview stops independently");
    p.pads[0].gainDb=0;
    auto tiny=onePad(1.0e-30); engine.publish(asset,tiny); p.pads[0].loop=true;
    out=render(engine,p,44100,512); check(engine.voicesActive(),"subframe loop is bounded and quantized to one frame");
    p.pads[0].loop=false; engine.publish(asset,state);
    for(int i=0;i<padCount;++i) { auto data=render(engine,p,44100,512,firstMidiNote+i); check(peak(data.left,100,500)>0.05f,"pad MIDI slot produces audio"); }
    auto stemAsset=tone(44100,0.5); stemAsset->stemsReady=true;
    for(auto& stem:stemAsset->stems) { stem=stemAsset->audio; stem.applyGain(0.25f); }
    engine.publish(stemAsset,onePad(.5)); p=defaults(); p.stemSolo[1]=true;
    out=render(engine,p,44100,4410); check(peak(out.left,500,4000)>.05f && peak(out.left,500,4000)<.2f,"solo plays only its aligned stem");
    p.stemSolo[1]=false; out=render(engine,p,44100,4410); check(peak(out.left,500,4000)>.4f,"clearing solo restores the original mix");
}

void dumpFixture(SliceLabAudioProcessor& processor, const char* filename) {
    const auto path=juce::SystemStats::getEnvironmentVariable("SLICELAB_FIXTURE_DIR",{});
    if(path.isEmpty()) return;
    const juce::File directory(path);
    if(!directory.createDirectory().wasOk()) { check(false,"fixture output directory is writable"); return; }
    juce::MemoryBlock state; processor.getStateInformation(state);
    check(directory.getChildFile(filename).replaceWithData(state.getData(),state.getSize()),"processor fixture is saved");
}
void polyphonyAndLifetimes() {
    auto old=tone(44100,1.0); std::weak_ptr<SampleAsset> retired=old; auto state=onePad(1); auto p=defaults();
    SamplerEngine engine; engine.prepare(44100,1024); engine.publish(old,state);
    juce::AudioBuffer<float> buffer(2,1024); juce::MidiBuffer midi; std::array<int,padCount> offsets {};
    midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)127),0); engine.process(buffer,midi,p,offsets);
    old.reset(); auto replacement=tone(44100,1.0,220); engine.publish(replacement,state); engine.collectRetired();
    midi.clear(); engine.process(buffer,midi,p,offsets); engine.collectRetired();
    check(!retired.expired() && buffer.getMagnitude(0,0,1024)>0.05f,"playing voice retains its original asset across replacement");
    for(int i=0;i<45;++i) engine.process(buffer,midi,p,offsets);
    engine.collectRetired(); check(retired.expired(),"retired audio is reclaimed after its last voice ends");

    for(auto& pad:state.pads) { pad.startSeconds=0; pad.endSeconds=1; }
    engine.publish(replacement,state); p.mono=false; p.hold=true;
    for(int i=0;i<padCount;++i) midi.addEvent(juce::MidiMessage::noteOn(1,firstMidiNote+i,(juce::uint8)127),0);
    engine.process(buffer,midi,p,offsets);
    bool allActive=true; for(int i=0;i<padCount;++i) allActive=allActive && engine.activity(i)>0;
    check(allActive,"all 32 pad voices can sound together");
    midi.clear(); midi.addEvent(juce::MidiMessage::allSoundOff(1),0); engine.process(buffer,midi,p,offsets);
    check(buffer.getMagnitude(0,0,1024)<.00001f,"all-sound-off clears active notes");
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)127),0);
    midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)127),50); midi.addEvent(juce::MidiMessage::noteOff(1,36),100);
    engine.process(buffer,midi,p,offsets); check(buffer.getMagnitude(0,700,300)>.05f,"one repeated-note release does not stop the later voice");
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOff(1,36),0); engine.process(buffer,midi,p,offsets);
    check(buffer.getMagnitude(0,700,300)<.00001f,"second repeated-note release ends the remaining voice");
    p.mono=true;
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)127),0); midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)127),50); midi.addEvent(juce::MidiMessage::noteOff(1,36),100);
    engine.process(buffer,midi,p,offsets); check(buffer.getMagnitude(0,700,300)>.05f,"mono retrigger keeps the newer note through the earlier note-off");
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOff(1,36),0); engine.process(buffer,midi,p,offsets);
    check(buffer.getMagnitude(0,700,300)<.00001f,"mono retrigger ends on its own note-off"); p.mono=false;
    midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)127),0);
    midi.addEvent(juce::MidiMessage::controllerEvent(1,64,127),5); midi.addEvent(juce::MidiMessage::noteOff(1,36),50);
    engine.process(buffer,midi,p,offsets); check(buffer.getMagnitude(0,700,300)>.05f,"sustain pedal holds a released note");
    midi.clear(); midi.addEvent(juce::MidiMessage::controllerEvent(1,64,0),0); engine.process(buffer,midi,p,offsets);
    check(buffer.getMagnitude(0,700,300)<.00001f,"sustain release ends held notes");
    for(int i=0;i<300;++i) engine.enqueueTrigger(0,1,true);
    midi.clear(); engine.process(buffer,midi,p,offsets);
    check(engine.commandOverflows()>0 && !engine.voicesActive(),"overflow stops voices rather than leaving a lost release stuck");
}

void analysisBehavior() {
    const auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("slicelab-analysis-"+juce::Uuid().toString());
    dir.createDirectory();
    const auto silent=dir.getChildFile("silence.wav"), pulse=dir.getChildFile("pulse.wav"), chord=dir.getChildFile("chord.wav");
    auto write=[](const juce::File& path,juce::AudioBuffer<float>& audio) {
        juce::WavAudioFormat format; std::unique_ptr<juce::OutputStream> stream(path.createOutputStream());
        auto options=juce::AudioFormatWriterOptions().withSampleRate(44100).withNumChannels(2).withBitsPerSample(24);
        auto writer=format.createWriterFor(stream,options);
        return writer && writer->writeFromAudioSampleBuffer(audio,0,audio.getNumSamples());
    };
    juce::AudioBuffer<float> audio(2,44100*6); audio.clear();
    check(write(silent,audio),"silence fixture is written");
    for(int beat=0;beat<12;++beat) for(int i=0;i<1200;++i) {
        const float x=(float)(std::exp(-i/300.0)*std::sin(2*juce::MathConstants<double>::pi*160*i/44100.0));
        for(int c=0;c<2;++c) audio.setSample(c,beat*22050+i,x);
    }
    check(write(pulse,audio),"tempo fixture is written");
    for(int i=0;i<audio.getNumSamples();++i) {
        const double t=i/44100.0;
        const float x=(float)(.2*(std::sin(2*juce::MathConstants<double>::pi*261.6256*t)+std::sin(2*juce::MathConstants<double>::pi*329.6276*t)+std::sin(2*juce::MathConstants<double>::pi*391.9954*t)));
        for(int c=0;c<2;++c) audio.setSample(c,i,x);
    }
    check(write(chord,audio),"key fixture is written");
    SliceLabAudioProcessor plugin; plugin.loadSample(silent);
    for(int i=0;i<1000 && plugin.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto blank=plugin.snapshot(); check(blank.asset && blank.asset->detectedBpm==0 && blank.asset->detectedKey=="Unknown","silence produces unknown tempo and key");
    plugin.loadSample(pulse); for(int i=0;i<1000 && plugin.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto rhythm=plugin.snapshot(); check(rhythm.asset && std::abs(rhythm.asset->detectedBpm-120)<4 && rhythm.asset->bpmConfidence>0,"onset analysis identifies a synthetic 120 BPM pulse");
    plugin.loadSample(chord); for(int i=0;i<1000 && plugin.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const auto tonal=plugin.snapshot(); check(tonal.asset && tonal.asset->detectedKey=="C major" && tonal.asset->keyConfidence>0,"chroma analysis identifies a synthetic C major chord");
    dir.deleteRecursively();
}

void processorState() {
    const auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("slicelab-tests-"+juce::Uuid().toString());
    dir.createDirectory();
    const auto temp=dir.getChildFile("fixture with spaces.wav");
    auto a=tone(44100,0.25); juce::WavAudioFormat format; auto stream=temp.createOutputStream();
    std::unique_ptr<juce::OutputStream> output(std::move(stream)); auto options=juce::AudioFormatWriterOptions().withSampleRate(44100).withNumChannels(2).withBitsPerSample(24);
    auto writer=format.createWriterFor(output,options); check((bool)writer,"generated WAV fixture can be written");
    if(!writer) return; writer->writeFromAudioSampleBuffer(a->audio,0,a->audio.getNumSamples()); writer.reset();
    SliceLabAudioProcessor first; first.prepareToPlay(48000,256); first.loadSample(temp);
    for(int i=0;i<200 && first.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    auto snap=first.snapshot(); check(snap.asset!=nullptr,"worker decodes sample"); if(!snap.asset) return;
    first.loadSample(temp.getSiblingFile("does-not-exist.wav"));
    for(int i=0;i<500 && first.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(first.snapshot().asset==snap.asset && first.snapshot().error.isNotEmpty(),"failed import preserves the playable source");
    auto pad=snap.session.pads[0]; pad.startSeconds=.02; pad.endSeconds=.2; pad.gainDb=-3; pad.reverse=true; first.setPad(0,pad);
    auto global=first.snapshot().session.global; global.sync=false; global.targetBpm=110; first.setGlobal(global);
    first.setBeatAnchors({{0,0},{.1,.25},{.2,.6}});
    pad=first.snapshot().session.pads[0]; pad.favorite=true; first.setPad(0,pad);
    global=first.snapshot().session.global; global.beatOffsetSeconds=.05; first.setGlobal(global); first.autoSlice(SliceMode::Grid,8,.5f);
    check(std::abs(first.snapshot().session.pads[1].endSeconds-.05)<.0001,"beat offset shifts the anchor-derived grid");
    first.autoSlice(SliceMode::Equal,8,.5f);
    check(std::abs(first.snapshot().session.pads[0].startSeconds-.02)<.00001 && first.snapshot().session.pads[0].favorite,"automatic slicing preserves favorites");
    if(auto* parameter=first.parameters.getParameter("pad01_gain")) parameter->setValueNotifyingHost(parameter->convertTo0to1(-9));
    check(std::abs(first.snapshot().session.pads[0].gainDb+9)<.01,"host automation updates the pad state");
    ViewState view; view.width=1310; view.height=850; first.setViewState(view);
    juce::MemoryBlock saved; first.getStateInformation(saved);
    const auto stemFile=temp.getSiblingFile("SliceLab gain fixture.wav"), manifestFile=temp.getSiblingFile("SliceLab gain fixture.json");
    auto stemAudio=a->audio; stemAudio.applyGain(.25f);
    auto stemStream=stemFile.createOutputStream(); std::unique_ptr<juce::OutputStream> stemOutput(std::move(stemStream));
    auto stemWriter=format.createWriterFor(stemOutput,options); if(stemWriter) { stemWriter->writeFromAudioSampleBuffer(stemAudio,0,stemAudio.getNumSamples()); stemWriter.reset(); }
    auto* manifestObject=new juce::DynamicObject(); auto* stemPaths=new juce::DynamicObject(); auto* files=new juce::DynamicObject(); auto* gains=new juce::DynamicObject();
    for(const auto* name:{"vocals","drums","bass","other"}) { stemPaths->setProperty(name,stemFile.getFullPathName()); auto* info=new juce::DynamicObject(); info->setProperty("sha256",juce::SHA256(stemFile).toHexString()); files->setProperty(name,juce::var(info)); }
    gains->setProperty("input",.5); gains->setProperty("output",.5);
    manifestObject->setProperty("schema",1); manifestObject->setProperty("input_sha256",juce::SHA256(temp).toHexString()); manifestObject->setProperty("sample_rate",44100); manifestObject->setProperty("frames",a->audio.getNumSamples()); manifestObject->setProperty("model","test"); manifestObject->setProperty("model_sha256",juce::String::repeatedString("0",64)); manifestObject->setProperty("stems",juce::var(stemPaths)); manifestObject->setProperty("files",juce::var(files)); manifestObject->setProperty("processing_gain",juce::var(gains));
    manifestFile.replaceWithText(juce::JSON::toString(juce::var(manifestObject)));
    auto gainXml=juce::AudioProcessor::getXmlFromBinary(saved.getData(),(int)saved.getSize()); gainXml->setAttribute("stemManifest",manifestFile.getFullPathName());
    juce::MemoryBlock gainState; juce::AudioProcessor::copyXmlToBinary(*gainXml,gainState); SliceLabAudioProcessor gainRestored; gainRestored.setStateInformation(gainState.getData(),(int)gainState.getSize());
    const auto gainAsset=gainRestored.snapshot().asset;
    check(gainAsset && gainAsset->stemsReady && std::abs(gainAsset->stems[0].getSample(0,30)-a->audio.getSample(0,30))<.0001f,"stem processing gain is reversed on import");
    const auto verifiedManifest=manifestFile.loadFileAsString();
    auto unsignedManifest=juce::JSON::parse(verifiedManifest);
    unsignedManifest.getDynamicObject()->removeProperty("files");
    manifestFile.replaceWithText(juce::JSON::toString(unsignedManifest));
    SliceLabAudioProcessor unverifiedStems;
    unverifiedStems.setStateInformation(gainState.getData(),(int)gainState.getSize());
    check(unverifiedStems.snapshot().asset && !unverifiedStems.snapshot().asset->stemsReady && unverifiedStems.snapshot().error.containsIgnoreCase("checksums"),"stem manifest without checksums cannot become ready");
    manifestFile.replaceWithText(verifiedManifest);
    stemFile.replaceWithText("changed stem"); const auto collectDir=temp.getSiblingFile("SliceLab failed collect fixture"); gainRestored.collectSample(collectDir);
    for(int i=0;i<500 && gainRestored.snapshot().error.isEmpty();++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(gainRestored.snapshot().error.containsIgnoreCase("changed") && gainRestored.snapshot().session.sourcePath==temp.getFullPathName(),"collect rejects a changed stem without relinking the session");
    stemFile.deleteFile(); manifestFile.deleteFile(); collectDir.deleteRecursively();
    SliceLabAudioProcessor restored; restored.prepareToPlay(48000,256); restored.setStateInformation(saved.getData(),(int)saved.getSize());
    const auto result=restored.snapshot(); check(result.asset && std::abs(result.session.pads[0].startSeconds-.02)<.00001 && result.session.pads[0].reverse && std::abs(result.session.global.targetBpm-110)<.01,"state restores source and pad parameters synchronously");
    check(result.session.beatAnchors.size()==3 && result.session.view.width==1310,"state restores beat anchors and view");
    juce::AudioBuffer<float> buffer(2,256); juce::MidiBuffer midi;
    auto compensatedPeak=[&](int noteOffset) {
        midi.clear(); midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)127),noteOffset);
        const int latency=restored.getLatencySamples(); float peak=0;
        for(int start=0;start<latency+256;start+=256) {
            restored.processBlock(buffer,midi); midi.clear();
            const int first=std::max(0,latency+38-start), last=std::min(256,latency+256-start);
            if(last>first) peak=std::max(peak,buffer.getMagnitude(0,first,last-first));
        }
        return peak;
    };
    const float originalPeak=compensatedPeak(37);
    check(originalPeak>0.001f,"restored processor renders its first note with declared PDC and no readiness wait");
    if(auto* parameter=restored.parameters.getParameter("pad01_gain")) parameter->setValueNotifyingHost(parameter->convertTo0to1(-60));
    check(compensatedPeak(0)<originalPeak*.01f,"host automation changes rendered pad gain after the same declared PDC");
    auto brokenXml=juce::AudioProcessor::getXmlFromBinary(saved.getData(),(int)saved.getSize());
    const auto missingManifest=temp.getSiblingFile("missing-stem-manifest.json").getFullPathName();
    brokenXml->setAttribute("stemManifest",missingManifest); juce::MemoryBlock brokenState;
    juce::AudioProcessor::copyXmlToBinary(*brokenXml,brokenState); SliceLabAudioProcessor brokenStems;
    brokenStems.setStateInformation(brokenState.getData(),(int)brokenState.getSize());
    juce::MemoryBlock savedAgain; brokenStems.getStateInformation(savedAgain);
    auto savedXml=juce::AudioProcessor::getXmlFromBinary(savedAgain.getData(),(int)savedAgain.getSize());
    check(savedXml && savedXml->getStringAttribute("stemManifest")==missingManifest,"missing stem manifest remains recoverable after another save");
    const auto before=restored.snapshot().session; const char bad[]="not a valid state"; restored.setStateInformation(bad,sizeof(bad));
    check(restored.snapshot().session.sourceHash==before.sourceHash,"corrupt state leaves existing session intact");
    const auto relocated=temp.getSiblingFile("SliceLab relink fixture.wav"); temp.copyFileTo(relocated); temp.deleteFile();
    SliceLabAudioProcessor missing; missing.setStateInformation(saved.getData(),(int)saved.getSize());
    check(!missing.snapshot().asset && missing.snapshot().session.sourcePath==temp.getFullPathName() && missing.snapshot().error.isNotEmpty(),"missing source preserves session and reports relink state");
    const auto wrong=temp.getSiblingFile("SliceLab wrong relink fixture.wav"); auto wrongTone=tone(44100,.25,220);
    auto wrongStream=wrong.createOutputStream(); std::unique_ptr<juce::OutputStream> wrongOutput(std::move(wrongStream));
    auto wrongWriter=format.createWriterFor(wrongOutput,options); if(wrongWriter) { wrongWriter->writeFromAudioSampleBuffer(wrongTone->audio,0,wrongTone->audio.getNumSamples()); wrongWriter.reset(); }
    brokenStems.loadSample(wrong); for(int i=0;i<500 && brokenStems.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10)); brokenStems.undo();
    juce::MemoryBlock undoState; brokenStems.getStateInformation(undoState); auto undoXml=juce::AudioProcessor::getXmlFromBinary(undoState.getData(),(int)undoState.getSize());
    check(undoXml && undoXml->getStringAttribute("stemManifest")==missingManifest,"undo restores the durable missing-stem reference");
    missing.relinkSample(wrong); for(int i=0;i<500 && missing.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(!missing.snapshot().asset && missing.snapshot().session.sourcePath==temp.getFullPathName() && missing.snapshot().error.isNotEmpty(),"mismatched relink is rejected without changing the session");
    missing.relinkSample(relocated); for(int i=0;i<500 && missing.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(missing.snapshot().asset && std::abs(missing.snapshot().session.pads[0].startSeconds-.02)<.00001 && missing.snapshot().session.beatAnchors.size()==3,"matching relink preserves slice and beat edits");
    dir.deleteRecursively();
}
}
int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    { SliceLabAudioProcessor factory; dumpFixture(factory,"factory.slicelab"); }
    engineBehavior(44100); engineBehavior(48000); engineBehavior(96000); continuousAutomation(); reverseAndPads(); polyphonyAndLifetimes(); processorState(); analysisBehavior();
    const juce::File real(juce::String(SLICELAB_TEST_ROOT)+"/sample_test.wav");
    if(real.existsAsFile()) {
        SliceLabAudioProcessor plugin; plugin.prepareToPlay(44100,256); plugin.loadSample(real);
        for(int i=0;i<3000 && plugin.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        check(plugin.snapshot().asset!=nullptr,"supplied sample decodes");
        if(plugin.snapshot().asset) {
            plugin.separateStems();
            for(int i=0;i<6000 && plugin.snapshot().busy;++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const auto separated=plugin.snapshot();
            check(separated.asset && separated.asset->stemsReady,"real worker result loads four aligned stems");
            if(!separated.error.isEmpty()) std::cerr << "Stem worker: " << separated.error << '\n';
            if(separated.asset && separated.asset->stemsReady) {
                dumpFixture(plugin,"sample.slicelab");
                auto g=separated.session.global; g.stemEnabled={false,false,false,false}; plugin.setGlobal(g);
                juce::AudioBuffer<float> buffer(2,256); juce::MidiBuffer midi; midi.addEvent(juce::MidiMessage::noteOn(1,36,(juce::uint8)127),0); plugin.processBlock(buffer,midi);
                check(buffer.getMagnitude(0,0,256)<0.000001f,"real separated sample is silent with all stems off");
            }
        }
    }
    std::cout << failures << " failure(s)\n"; return failures==0?0:1;
}
