#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SamplerEngine.h"
#include "MarkerImport.h"
#include <juce_cryptography/juce_cryptography.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <numeric>
#include <spawn.h>
#include <set>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <thread>

extern char **environ;

using namespace slicelab;
namespace {
constexpr int stemCountLocal = 4;
constexpr std::size_t maxSourceBytes = 128u * 1024u * 1024u;
constexpr std::size_t maxAssetBytes = 256u * 1024u * 1024u;
constexpr std::size_t maxHistoryBytes = 256u * 1024u * 1024u;
const std::array<const char*, stemCountLocal> stemNames { "vocals", "drums", "bass", "other" };
constexpr double pi = 3.14159265358979323846;
juce::String padId(int i, const char* property) { return "pad" + juce::String(i + 1).paddedLeft('0', 2) + "_" + property; }
juce::String stemId(int i, const char* property) { return juce::String("stem_") + stemNames[(size_t) i] + "_" + property; }
float clampFloat(float value, float low, float high, float fallback) { return std::isfinite(value) ? std::clamp(value, low, high) : fallback; }
double clampDouble(double value, double low, double high, double fallback) { return std::isfinite(value) ? std::clamp(value, low, high) : fallback; }

int keyClass(const juce::String& name) {
    const auto tonic=name.upToFirstOccurrenceOf(" ",false,false);
    static const std::array<juce::String,12> sharp { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    static const std::array<juce::String,12> flat { "C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B" };
    for(int i=0;i<12;++i) if(tonic==sharp[(size_t)i] || tonic==flat[(size_t)i]) return i+(name.containsIgnoreCase("minor")?12:0);
    return -1;
}

std::size_t pcmBytes(const SampleAsset& asset) {
    auto bytes=[](const juce::AudioBuffer<float>& audio) { return (std::size_t)audio.getNumChannels() * (std::size_t)audio.getNumSamples() * sizeof(float); };
    std::size_t total=bytes(asset.audio);
    for(const auto& stem:asset.stems) total+=bytes(stem);
    return total;
}

std::shared_ptr<SampleAsset> copyWithoutStems(const SampleAsset& source) {
    auto result=std::make_shared<SampleAsset>();
    result->audio=source.audio; result->sampleRate=source.sampleRate;
    result->sourcePath=source.sourcePath; result->sourceHash=source.sourceHash; result->generation=source.generation;
    result->peaks=source.peaks; result->transients=source.transients;
    result->detectedBpm=source.detectedBpm; result->bpmConfidence=source.bpmConfidence;
    result->detectedKey=source.detectedKey; result->camelot=source.camelot;
    result->keyConfidence=source.keyConfidence; result->keyConfidenceLabel=source.keyConfidenceLabel;
    return result;
}

void quantizeBounds(PadSettings& pad, double rate, int frames) {
    if (!(rate > 0) || frames <= 0 || !pad.active()) return;
    const auto first=(int)std::clamp(std::round(pad.startSeconds*rate),0.0,(double)frames);
    auto last=(int)std::clamp(std::round(pad.endSeconds*rate),0.0,(double)frames);
    if(last<=first && first<frames) last=first+1;
    pad.startSeconds=first/rate; pad.endSeconds=last/rate;
}

PadSettings cleanPad(PadSettings p, double duration) {
    p.startSeconds = clampDouble(p.startSeconds, 0, duration, 0);
    p.endSeconds = clampDouble(p.endSeconds, 0, duration, 0);
    if (p.endSeconds < p.startSeconds) p.endSeconds = p.startSeconds;
    p.gainDb = clampFloat(p.gainDb, -60, 12, 0); p.pan = clampFloat(p.pan, -1, 1, 0);
    p.semitones = clampFloat(p.semitones, -24, 24, 0); p.cents = clampFloat(p.cents, -100, 100, 0);
    p.timeRatio = clampFloat(p.timeRatio, 0.25f, 4, 1); p.cutoffHz = clampFloat(p.cutoffHz, 20, 20000, 20000);
    p.resonance = clampFloat(p.resonance, 0.1f, 10, 0.7071f); p.attackMs = clampFloat(p.attackMs, 0, 2000, 2);
    p.releaseMs = clampFloat(p.releaseMs, 0, 5000, 25); p.output = std::clamp(p.output, 0, 31);
    p.name = p.name.substring(0, 80);
    return p;
}
GlobalSettings cleanGlobal(GlobalSettings g) {
    g.sourceBpm = clampDouble(g.sourceBpm, 20, 300, 120); g.targetBpm = clampDouble(g.targetBpm, 20, 300, 120);
    g.beatOffsetSeconds = clampDouble(g.beatOffsetSeconds, -3600, 3600, 0);
    g.gridDivisionBeats = clampDouble(g.gridDivisionBeats, 0.0625, 4.0, 1.0);
    g.gainDb = clampFloat(g.gainDb, -60, 12, -6); g.semitones = clampFloat(g.semitones, -24, 24, 0);
    g.cents = clampFloat(g.cents, -100, 100, 0); g.hostMultiplier = clampFloat(g.hostMultiplier, 0.25f, 4, 1);
    g.rootMidiNote = std::clamp(g.rootMidiNote, 0, 127); g.glideMs=clampFloat(g.glideMs,0,2000,0); g.keyOverride=std::clamp(g.keyOverride,-1,23);
    for (auto& gain : g.stemGainDb) gain = clampFloat(gain, -60, 12, 0);
    return g;
}

std::shared_ptr<SampleAsset> decodeSample(const juce::File& file, std::uint64_t generation, juce::String& error) {
    if (!file.existsAsFile()) { error = "Sample file is missing: " + file.getFullPathName(); return {}; }
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader || reader->lengthInSamples <= 0 || !std::isfinite(reader->sampleRate) || reader->sampleRate < 8000 || reader->sampleRate > 192000 || reader->numChannels < 1 || reader->numChannels > 32) {
        error = "Unsupported or empty audio file"; return {};
    }
    if(reader->lengthInSamples > (juce::int64)(maxSourceBytes / (2 * sizeof(float)))) { error="Sample exceeds the 128 MiB decoded audio limit"; return {}; }
    const auto initialHash = juce::SHA256(file).toHexString();
    auto asset = std::make_shared<SampleAsset>();
    asset->audio.setSize(2, (int) reader->lengthInSamples);
    if (!reader->read(&asset->audio, 0, (int) reader->lengthInSamples, 0, true, true)) { error = "Could not decode sample audio"; return {}; }
    if (reader->numChannels == 1) asset->audio.copyFrom(1, 0, asset->audio, 0, 0, asset->audio.getNumSamples());
    for (int channel=0; channel<2; ++channel) { auto* samples=asset->audio.getWritePointer(channel); for (int i=0; i<asset->audio.getNumSamples(); ++i) samples[i]=std::isfinite(samples[i]) ? std::clamp(samples[i],-1.0e6f,1.0e6f) : 0.0f; }
    asset->sampleRate = reader->sampleRate; asset->sourcePath = file.getFullPathName();
    asset->sourceHash = juce::SHA256(file).toHexString();
    if (asset->sourceHash != initialHash) { error = "Sample changed while it was being loaded"; return {}; }
    asset->generation = generation;
    return asset;
}

juce::String mediaSizeError(const juce::File& file, bool hasStems) {
    if(!file.existsAsFile()) return {};
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if(!reader || reader->lengthInSamples<=0) return {};
    if(reader->lengthInSamples>(juce::int64)(maxSourceBytes/(2u*sizeof(float)))) return "Sample exceeds the 128 MiB decoded audio limit";
    if(hasStems && reader->lengthInSamples>(juce::int64)(maxAssetBytes/((stemCount+1u)*2u*sizeof(float)))) return "Sample is too large to restore four stems";
    return {};
}

void analyse(SampleAsset& a) {
    const int frames = a.audio.getNumSamples(); const double rate = a.sampleRate;
    constexpr int bins = 2048;
    const int peakStep = std::max(1, (frames + bins - 1) / bins);
    for (int start = 0; start < frames; start += peakStep) {
        float low = 1, high = -1;
        for (int i = start; i < std::min(frames, start + peakStep); ++i) {
            const float left=a.audio.getSample(0,i), right=a.audio.getSample(1,i);
            low = std::min({low,left,right}); high = std::max({high,left,right});
        }
        a.peaks.push_back({low, high});
    }
    constexpr int hop = 512;
    std::vector<float> envelope((size_t) ((frames + hop - 1) / hop), 0.0f);
    double energy = 0, midEnergy = 0;
    for (int i = 0; i < frames; ++i) {
        const float left=a.audio.getSample(0,i),right=a.audio.getSample(1,i),mid=.5f*(left+right);
        const float power=.5f*(left*left+right*right);
        envelope[(size_t) (i / hop)] += power; energy += power; midEnergy += mid*mid;
    }
    if (energy / std::max(1, frames) < 1.0e-10) return;
    const bool analyseLeftOnly=midEnergy < energy * 0.1;
    for (auto& value : envelope) value = std::sqrt(value / hop);
    std::vector<float> onset(envelope.size(), 0);
    for (size_t i = 1; i < envelope.size(); ++i) onset[i] = std::max(0.0f, envelope[i] - 0.8f * envelope[i - 1]);
    const float maxOnset = *std::max_element(onset.begin(), onset.end());
    const int minDistance = std::max(1, (int) (0.08 * rate / hop));
    int last = -minDistance;
    for (int i = 1; i + 1 < (int) onset.size(); ++i) {
        if (onset[(size_t) i] > maxOnset * 0.12f && onset[(size_t) i] > onset[(size_t) i - 1] && onset[(size_t) i] >= onset[(size_t) i + 1] && i - last >= minDistance) {
            a.transients.push_back(i * hop / rate); last = i;
        }
    }
    if (a.transients.size() >= 3) {
        const int lo = std::max(1, (int) (rate * 60.0 / (200 * hop)));
        const int hi = std::min((int) onset.size() / 2, (int) (rate * 60.0 / (60 * hop)));
        double best = 0, zero = 0; int bestLag = 0;
        for (float x : onset) zero += x * x;
        for (int lag = lo; lag <= hi; ++lag) {
            double score = 0;
            for (int i = lag; i < (int) onset.size(); ++i) score += onset[(size_t) i] * onset[(size_t) (i - lag)];
            if (score > best) { best = score; bestLag = lag; }
        }
        if (bestLag > 0 && zero > 0) {
            a.bpmConfidence = (float) std::clamp(best / zero, 0.0, 1.0);
            if (a.bpmConfidence >= 0.12f) a.detectedBpm = 60.0 * rate / (bestLag * hop);
        }
    }
    constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft(fftOrder);
    std::array<float, fftSize * 2> data {};
    std::array<double, 12> chroma {};
    const int stride = std::max(fftSize / 2, frames / (fftSize / 2 * 400) * (fftSize / 2));
    for (int offset = 0; offset + fftSize <= frames; offset += stride) {
        for (int i = 0; i < fftSize; ++i) {
            const float left=a.audio.getSample(0,offset+i), right=a.audio.getSample(1,offset+i);
            const float x = analyseLeftOnly ? left : 0.5f * (left + right);
            data[(size_t) i] = x * (float) (0.5 - 0.5 * std::cos(2 * pi * i / (fftSize - 1)));
        }
        std::fill(data.begin() + fftSize, data.end(), 0);
        fft.performFrequencyOnlyForwardTransform(data.data());
        for (int k = 2; k < fftSize / 2 - 1; ++k) {
            const double hz = k * rate / fftSize;
            if (hz < 65 || hz > 4000 || data[(size_t) k] <= data[(size_t) k - 1] || data[(size_t) k] <= data[(size_t) k + 1]) continue;
            const int note = (int) std::lround(69 + 12 * std::log2(hz / 440.0));
            chroma[(size_t) ((note % 12 + 12) % 12)] += data[(size_t) k];
        }
    }
    constexpr std::array<double, 12> major { 6.35,2.23,3.48,2.33,4.38,4.09,2.52,5.19,2.39,3.66,2.29,2.88 };
    constexpr std::array<double, 12> minor { 6.33,2.68,3.52,5.38,2.60,3.53,2.54,4.75,3.98,2.69,3.34,3.17 };
    const double sum = std::accumulate(chroma.begin(), chroma.end(), 0.0);
    if (sum <= 0) return;
    double best = -1e9, second = -1e9; int key = 0; bool isMinor = false;
    for (int mode = 0; mode < 2; ++mode) for (int root = 0; root < 12; ++root) {
        double score = 0;
        for (int i = 0; i < 12; ++i) score += (chroma[(size_t) ((i + root) % 12)] / sum) * (mode ? minor : major)[(size_t) i];
        if (score > best) { second = best; best = score; key = root; isMinor = mode != 0; }
        else second = std::max(second, score);
    }
    a.keyConfidence = (float) std::clamp((best - second) / std::max(0.1, best) * 4.0, 0.0, 1.0);
    if (a.keyConfidence >= 0.05f) {
        static constexpr const char* notes[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
        static constexpr const char* majorCamelot[] = { "8B", "3B", "10B", "5B", "12B", "7B", "2B", "9B", "4B", "11B", "6B", "1B" };
        static constexpr const char* minorCamelot[] = { "5A", "12A", "7A", "2A", "9A", "4A", "11A", "6A", "1A", "8A", "3A", "10A" };
        a.detectedKey = juce::String(notes[key]) + (isMinor ? " minor" : " major");
        a.camelot = isMinor ? minorCamelot[key] : majorCamelot[key];
    }
}

std::vector<double> suggestBoundaries(const SampleAsset& asset, int count, float sensitivity) {
    struct Feature { double time, energy, brightness; };
    struct Candidate { double time, score; };
    const int frames=asset.audio.getNumSamples();
    const int window=std::max(128,(int)std::round(asset.sampleRate * 0.02));
    const int stride=std::max(1,frames / 1500000);
    std::vector<Feature> features;
    features.reserve((size_t)(frames/window+1));
    for(int start=0;start<frames;start+=window) {
        double power=0, difference=0, magnitude=0;
        float last=0; int n=0;
        for(int i=start;i<std::min(frames,start+window);i+=stride) {
            const float left=asset.audio.getSample(0,i), right=asset.audio.getSample(1,i);
            const float sample=std::abs(left)>=std::abs(right) ? left : right;
            power+=0.5*((double)left*left+(double)right*right);
            difference+=std::abs(sample-last); magnitude+=std::abs(sample); last=sample; ++n;
        }
        features.push_back({start/asset.sampleRate,std::sqrt(power/std::max(1,n)),difference/std::max(1.0e-9,magnitude)});
    }
    std::vector<Candidate> candidates;
    for(size_t i=1;i+1<features.size();++i) {
        const auto& current=features[i]; const auto& before=features[i-1];
        const double contrast=std::max(0.0,current.energy-before.energy);
        const double novelty=std::abs(current.brightness-before.brightness)*std::min(current.energy,before.energy);
        const double score=contrast*2.0+novelty*0.3+current.energy*0.08;
        if(score>0 && current.energy>=features[i+1].energy*0.8)
            candidates.push_back({current.time,score});
    }
    std::stable_sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){return a.score>b.score;});
    std::vector<double> result;
    const double duration=asset.durationSeconds();
    const double spacing=std::max(0.08,std::min(0.5,duration/std::max(1,count*3)));
    const double threshold=candidates.empty()?0.0:candidates.front().score*(0.03+0.20*(1.0-std::clamp((double)sensitivity,0.0,1.0)));
    for(const auto& candidate:candidates) {
        if((int)result.size()>=count-1 || candidate.score<threshold) break;
        if(candidate.time<spacing || duration-candidate.time<spacing) continue;
        if(std::all_of(result.begin(),result.end(),[&](double time){return std::abs(time-candidate.time)>=spacing;})) result.push_back(candidate.time);
    }
    std::sort(result.begin(),result.end());
    return result;
}

struct ExternalKeyResult { juce::String key, camelot, confidence; };

ExternalKeyResult externalKey(const SampleAsset& asset, const std::function<bool()>& cancelled) {
    if(asset.durationSeconds()<0.75 || asset.durationSeconds()>180.0 || cancelled()) return {};
    const auto overridePath=juce::SystemStats::getEnvironmentVariable("SLICELAB_KEY_ANALYZER",{});
    if(overridePath.equalsIgnoreCase("off")) return {};
    const auto install=juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".local/share/reaper-key-detection");
    const juce::File script=overridePath.isNotEmpty() ? juce::File(overridePath) : install.getChildFile("analyze_key.py");
    const auto pythonOverride=juce::SystemStats::getEnvironmentVariable("SLICELAB_KEY_PYTHON",{});
    const juce::File python=pythonOverride.isNotEmpty() ? juce::File(pythonOverride) : script.getParentDirectory().getChildFile(".venv/bin/python");
    const juce::File source(asset.sourcePath);
    if(!script.existsAsFile() || !python.existsAsFile() || !source.existsAsFile() || juce::SHA256(source).toHexString()!=asset.sourceHash || cancelled()) return {};
    int output[2]; if(pipe(output)!=0) return {};
    fcntl(output[0],F_SETFL,fcntl(output[0],F_GETFL,0)|O_NONBLOCK);
    std::vector<std::string> args { python.getFullPathName().toStdString(),script.getFullPathName().toStdString(),asset.sourcePath.toStdString(),"--format","tsv" };
    std::vector<char*> argv; for(auto& arg:args) argv.push_back(arg.data()); argv.push_back(nullptr);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions,output[1],STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions,output[0]); posix_spawn_file_actions_addclose(&actions,output[1]);
    posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
    pid_t pid=-1; const int started=posix_spawn(&pid,args[0].c_str(),&actions,nullptr,argv.data(),environ);
    posix_spawn_file_actions_destroy(&actions); close(output[1]);
    if(started!=0) { close(output[0]); return {}; }
    std::string text; int status=0; bool interrupted=false; bool exited=false;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(45);
    auto drain=[&] { char buffer[4096]; ssize_t n=0; while((n=read(output[0],buffer,sizeof(buffer)))>0) if(text.size()<65536) text.append(buffer,std::min((size_t)n,65536-text.size())); };
    while(!exited) {
        drain();
        const auto result=waitpid(pid,&status,WNOHANG);
        if(result==pid) { exited=true; break; }
        if(result<0) { interrupted=true; break; }
        if(cancelled() || std::chrono::steady_clock::now()>=deadline) {
            interrupted=true; kill(pid,SIGTERM);
            for(int i=0;i<10;++i) { if(waitpid(pid,&status,WNOHANG)==pid) { exited=true; break; } std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
            if(!exited) { kill(pid,SIGKILL); waitpid(pid,&status,0); }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    drain(); close(output[0]);
    if(interrupted || !WIFEXITED(status) || WEXITSTATUS(status)!=0 || cancelled() || juce::SHA256(source).toHexString()!=asset.sourceHash) return {};
    const auto lines=juce::StringArray::fromLines(juce::String::fromUTF8(text.data(),(int)text.size()));
    for(const auto& line:lines) if(line.startsWith("KEY_RESULT\t")) {
        ExternalKeyResult result;
        const auto fields=juce::StringArray::fromTokens(line,"\t","");
        for(const auto& field:fields) {
            if(field.startsWith("key=")) result.key=field.fromFirstOccurrenceOf("=",false,false);
            else if(field.startsWith("camelot=")) result.camelot=field.fromFirstOccurrenceOf("=",false,false);
            else if(field.startsWith("confidence=")) result.confidence=field.fromFirstOccurrenceOf("=",false,false);
        }
        if(keyClass(result.key)>=0 && (result.key.endsWith(" major") || result.key.endsWith(" minor")) &&
           (result.confidence=="low" || result.confidence=="medium" || result.confidence=="high")) return result;
    }
    return {};
}

// A compressed source can decode to a different timeline in JUCE and FFmpeg.
// Write the immutable, already-decoded playback samples so the model sees the
// exact same frames as the sampler. WAV sources retain their existing pipeline.
juce::File canonicalSeparationInput(const SampleAsset& asset, const std::function<bool()>& cancelled,
                                    juce::String& transportHash, juce::String& error) {
    const auto directory=juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".cache/slicelab/separation-inputs");
    if(!directory.createDirectory().wasOk()) { error="Could not create the separation input folder"; return {}; }
    const auto target=directory.getChildFile(asset.sourceHash+".wav");
    const auto temporary=directory.getChildFile("."+asset.sourceHash+"."+juce::Uuid().toString()+".tmp.wav");
    struct Cleanup { juce::File file; ~Cleanup() { file.deleteFile(); } } cleanup {temporary};
    if(cancelled()) { error="Stem separation cancelled"; return {}; }
    std::unique_ptr<juce::OutputStream> stream(temporary.createOutputStream());
    juce::WavAudioFormat format;
    const auto options=juce::AudioFormatWriterOptions().withSampleRate(asset.sampleRate).withNumChannels(2).withBitsPerSample(32)
        .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer=stream ? format.createWriterFor(stream,options) : nullptr;
    if(!writer) { error="Could not create the separation input"; return {}; }
    constexpr int block=16384;
    for(int start=0;start<asset.audio.getNumSamples();start+=block) {
        if(cancelled()) { error="Stem separation cancelled"; return {}; }
        if(!writer->writeFromAudioSampleBuffer(asset.audio,start,std::min(block,asset.audio.getNumSamples()-start))) {
            error="Could not write the separation input"; return {};
        }
    }
    if(!writer->flush()) { error="Could not finish the separation input"; return {}; }
    writer.reset();

    auto input=temporary.createInputStream();
    std::unique_ptr<juce::AudioFormatReader> reader(input ? format.createReaderFor(input.release(),true) : nullptr);
    if(!reader || !reader->usesFloatingPointData || reader->bitsPerSample!=32 || reader->numChannels!=2 ||
       reader->lengthInSamples!=asset.audio.getNumSamples() || std::abs(reader->sampleRate-asset.sampleRate)>0.001) {
        error="Could not verify the separation input format"; return {};
    }
    juce::AudioBuffer<float> check(2,block);
    for(int start=0;start<asset.audio.getNumSamples();start+=block) {
        if(cancelled()) { error="Stem separation cancelled"; return {}; }
        const int count=std::min(block,asset.audio.getNumSamples()-start);
        if(!reader->read(&check,0,count,start,true,true)) { error="Could not verify the separation input audio"; return {}; }
        for(int channel=0;channel<2;++channel)
            if(std::memcmp(check.getReadPointer(channel),asset.audio.getReadPointer(channel,start),(size_t)count*sizeof(float))!=0) {
                error="Separation input audio does not match the loaded sample"; return {};
            }
    }
    reader.reset();
    transportHash=juce::SHA256(temporary).toHexString();
    // rename(2) replaces the destination atomically; a worker never sees a
    // partially written input, even when several plugin instances share it.
    if(std::rename(temporary.getFullPathName().toRawUTF8(),target.getFullPathName().toRawUTF8())!=0) {
        error="Could not publish the separation input"; return {};
    }
    return target;
}

bool bindSeparationManifest(const SampleAsset& asset, const juce::File& workerResult, const juce::File& boundResult,
                            const juce::String& transportHash, juce::String& error) {
    auto json=juce::JSON::parse(workerResult); auto* object=json.getDynamicObject();
    if(!object || (int)object->getProperty("schema")!=1 || object->getProperty("input_sha256").toString()!=transportHash ||
       (int)object->getProperty("sample_rate")!=(int)asset.sampleRate || (int)object->getProperty("channels")!=2 ||
       (juce::int64)object->getProperty("frames")!=asset.audio.getNumSamples()) {
        error="Stem result does not match the separation input"; return false;
    }
    object->setProperty("transport_input_sha256",transportHash);
    object->setProperty("input_sha256",asset.sourceHash);
    object->setProperty("source_path",asset.sourcePath);
    object->setProperty("source_decoding","JUCE 32-bit floating-point stereo timeline");
    if(!boundResult.replaceWithText(juce::JSON::toString(json))) { error="Could not save the verified stem manifest"; return false; }
    return true;
}

bool readStemFiles(SampleAsset& asset, const juce::String& manifestPath, juce::String& error) {
    const auto sourceBytes=(std::size_t)asset.audio.getNumSamples() * 2u * sizeof(float);
    if(sourceBytes > maxAssetBytes / (stemCount + 1u)) { error="Sample is too large to load four stems"; return false; }
    if (!juce::File::isAbsolutePath(manifestPath)) { error = "Invalid stem manifest path"; return false; }
    const juce::File file(manifestPath); const auto json = juce::JSON::parse(file);
    auto* object = json.getDynamicObject();
    if (!object || (int) object->getProperty("schema") != 1 || object->getProperty("input_sha256").toString() != asset.sourceHash ||
        (int) object->getProperty("sample_rate") != (int) asset.sampleRate || (juce::int64) object->getProperty("frames") != asset.audio.getNumSamples()) {
        error = "Stem result does not match the loaded sample"; return false;
    }
    auto* stems = object->getProperty("stems").getDynamicObject();
    if (!stems || object->getProperty("model").toString().isEmpty() || object->getProperty("model_sha256").toString().length() != 64) { error = "Invalid stem manifest"; return false; }
    auto* files = object->getProperty("files").getDynamicObject();
    if (!files) { error = "Stem manifest has no verified file checksums"; return false; }
    double correction=1.0;
    if (auto* gain=object->getProperty("processing_gain").getDynamicObject()) {
        const double inputGain=(double)gain->getProperty("input"), outputGain=(double)gain->getProperty("output");
        if(!std::isfinite(inputGain)||!std::isfinite(outputGain)||inputGain<1.0e-6||outputGain<1.0e-6||inputGain>1.000001||outputGain>1.000001) { error="Invalid stem processing gain"; return false; }
        correction=1.0/(inputGain*outputGain);
    }
    juce::AudioFormatManager formats; formats.registerBasicFormats();
    std::array<juce::AudioBuffer<float>,stemCount> decoded;
    std::array<juce::String,stemCount> paths;
    for (int s = 0; s < stemCount; ++s) {
        const auto path = stems->getProperty(stemNames[(size_t) s]).toString();
        if (!juce::File::isAbsolutePath(path)) { error = "Invalid stem path"; return false; }
        juce::File stemFile(path);
        if (!stemFile.existsAsFile()) {
            const auto relocated=file.getParentDirectory().getChildFile(stemFile.getFileName());
            if (relocated.existsAsFile()) stemFile=relocated;
        }
        auto* info = files->getProperty(stemNames[(size_t)s]).getDynamicObject();
        if (!info) { error = "Stem manifest has no verified file checksums"; return false; }
        const auto digest = info->getProperty("sha256").toString();
        if (digest.length()!=64 || juce::SHA256(stemFile).toHexString()!=digest) { error="Stem file checksum failed"; return false; }
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(stemFile));
        if (!reader || reader->lengthInSamples != asset.audio.getNumSamples() || !std::isfinite(reader->sampleRate) || std::abs(reader->sampleRate-asset.sampleRate)>0.001 || reader->numChannels < 1 || reader->numChannels > 2) { error = "Stem audio is missing or misaligned"; return false; }
        auto& audio = decoded[(size_t) s]; audio.setSize(2, asset.audio.getNumSamples());
        if (!reader->read(&audio, 0, audio.getNumSamples(), 0, true, true)) { error = "Could not decode stem audio"; return false; }
        if (reader->numChannels == 1) audio.copyFrom(1, 0, audio, 0, 0, audio.getNumSamples());
        for (int channel=0; channel<2; ++channel) { const auto* samples=audio.getReadPointer(channel); for (int i=0; i<audio.getNumSamples(); ++i) if(!std::isfinite(samples[i])) { error="Stem contains invalid audio"; return false; } }
        audio.applyGain((float)correction);
        for(int channel=0;channel<2;++channel) { const auto* samples=audio.getReadPointer(channel); for(int i=0;i<audio.getNumSamples();++i) if(!std::isfinite(samples[i])) { error="Stem gain produced invalid audio"; return false; } }
        paths[(size_t) s] = stemFile.getFullPathName();
    }
    asset.stems=std::move(decoded); asset.stemPaths=std::move(paths);
    asset.stemsReady = true; asset.stemModel = object->getProperty("model").toString(); asset.stemManifestPath = manifestPath;
    return true;
}
}

juce::AudioProcessorValueTreeState::ParameterLayout SliceLabAudioProcessor::createParameterLayout() {
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    auto addFloat = [&](const juce::String& id, const juce::String& name, float lo, float hi, float initial) {
        layout.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID(id, 1), name, juce::NormalisableRange<float>(lo, hi), initial));
    };
    auto addBool = [&](const juce::String& id, const juce::String& name, bool initial) {
        layout.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID(id, 1), name, initial));
    };
    auto addInt = [&](const juce::String& id, const juce::String& name, int lo, int hi, int initial) {
        layout.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID(id, 1), name, lo, hi, initial));
    };
    addFloat("master_gain", "Master gain", -60, 12, -6);
    addFloat("source_bpm", "Source BPM", 20, 300, 120); addFloat("target_bpm", "Target BPM", 20, 300, 120);
    addFloat("global_semitones", "Global semitones", -24, 24, 0); addFloat("global_cents", "Global cents", -100, 100, 0);
    addFloat("host_multiplier", "Host tempo multiplier", 0.25f, 4, 1); addBool("sync", "Tempo sync", true);
    addBool("hold", "Hold mode", false); addBool("mono", "Mono mode", true); addBool("keyboard", "Keyboard mode", false);
    addBool("repitch", "Repitch mode", false); addBool("velocity_sensitive", "Velocity sensitive", true); addFloat("glide_ms", "Keyboard glide", 0, 2000, 0);
    addInt("root_note", "Root MIDI note", 0, 127, 60);
    for (int s = 0; s < stemCount; ++s) {
        addBool(stemId(s, "enabled"), juce::String(stemNames[(size_t) s]) + " enabled", true);
        addFloat(stemId(s, "gain"), juce::String(stemNames[(size_t) s]) + " gain", -60, 12, 0);
        addBool(stemId(s, "solo"), juce::String(stemNames[(size_t) s]) + " solo", false);
    }
    for (int i = 0; i < padCount; ++i) {
        const auto prefix = "Pad " + juce::String(i + 1) + " ";
        addFloat(padId(i,"gain"), prefix + "gain", -60, 12, 0); addFloat(padId(i,"pan"), prefix + "pan", -1, 1, 0);
        addFloat(padId(i,"semitones"), prefix + "semitones", -24, 24, 0); addFloat(padId(i,"cents"), prefix + "cents", -100, 100, 0);
        addFloat(padId(i,"time"), prefix + "time ratio", 0.25f, 4, 1); addFloat(padId(i,"cutoff"), prefix + "cutoff", 20, 20000, 20000);
        addFloat(padId(i,"resonance"), prefix + "resonance", 0.1f, 10, 0.7071f);
        addFloat(padId(i,"attack"), prefix + "attack", 0, 2000, 2); addFloat(padId(i,"release"), prefix + "release", 0, 5000, 25);
        addBool(padId(i,"reverse"), prefix + "reverse", false); addBool(padId(i,"loop"), prefix + "loop", false);
        addInt(padId(i,"output"), prefix + "output", 0, 31, 0);
    }
    addBool("metronome", "Source preview metronome", false);
    return layout;
}

struct SliceLabAudioProcessor::Impl {
    struct PadRefs { std::atomic<float> *gain, *pan, *semitones, *cents, *time, *cutoff, *resonance, *attack, *release, *reverse, *loop, *output; };
    struct Entry { SessionState state; std::shared_ptr<const SampleAsset> asset; juce::String manifest; };
    SliceLabAudioProcessor& owner;
    SamplerEngine engine;
    mutable std::mutex mutex;
    SessionState session;
    std::shared_ptr<const SampleAsset> asset;
    std::vector<Entry> previous, next;
    int gestureDepth = 0;
    bool gestureRemembered = false;
    int selectedPad = 0;
    bool busy = false;
    float progress = -1;
    juce::String status = "Drop a sample to begin", error, pendingManifest;
    std::atomic<double> hostBpm { 0 };
    std::atomic<std::uint64_t> generation { 0 };
    std::atomic<bool> cancelled { false };
    std::mutex jobsMutex;
    std::condition_variable jobsChanged;
    enum class JobKind { General, Load, Key };
    struct Job { std::function<void()> run; JobKind kind; };
    std::deque<Job> jobs;
    bool stopping = false;
    std::thread worker;
    std::array<PadRefs, padCount> padRefs;
    std::array<std::atomic<float>*, stemCount> stemEnabled {}, stemGain {}, stemSolo {};
    std::atomic<float> *master, *sourceBpm, *targetBpm, *globalSemi, *globalCents, *multiplier, *sync, *hold, *mono, *keyboard, *rootNote, *repitch, *velocitySensitive, *glideMs, *metronome;

    explicit Impl(SliceLabAudioProcessor& p) : owner(p) {
        auto ref = [&](const juce::String& id) { auto* value = owner.parameters.getRawParameterValue(id); jassert(value != nullptr); return value; };
        master=ref("master_gain"); sourceBpm=ref("source_bpm"); targetBpm=ref("target_bpm"); globalSemi=ref("global_semitones"); globalCents=ref("global_cents"); multiplier=ref("host_multiplier");
        sync=ref("sync"); hold=ref("hold"); mono=ref("mono"); keyboard=ref("keyboard"); rootNote=ref("root_note"); repitch=ref("repitch"); velocitySensitive=ref("velocity_sensitive"); glideMs=ref("glide_ms");
        metronome=ref("metronome");
        for (int s=0;s<stemCount;++s) { stemEnabled[(size_t)s]=ref(stemId(s,"enabled")); stemGain[(size_t)s]=ref(stemId(s,"gain")); stemSolo[(size_t)s]=ref(stemId(s,"solo")); }
        for (int i=0;i<padCount;++i) padRefs[(size_t)i] = { ref(padId(i,"gain")), ref(padId(i,"pan")), ref(padId(i,"semitones")), ref(padId(i,"cents")), ref(padId(i,"time")), ref(padId(i,"cutoff")), ref(padId(i,"resonance")), ref(padId(i,"attack")), ref(padId(i,"release")), ref(padId(i,"reverse")), ref(padId(i,"loop")), ref(padId(i,"output")) };
        for (int i=0;i<padCount;++i) session.pads[(size_t)i].name = "Slice " + juce::String(i+1);
        engine.publish({}, session);
        worker = std::thread([this] {
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(jobsMutex);
                    if (!jobsChanged.wait_for(lock, std::chrono::seconds(1), [&] { return stopping || !jobs.empty(); })) {
                        lock.unlock(); engine.collectRetired(); continue;
                    }
                    if (stopping && jobs.empty()) break;
                    job = std::move(jobs.front().run); jobs.pop_front();
                }
                const auto startedGeneration=generation.load();
                try { job(); }
                catch(const std::bad_alloc&) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(startedGeneration==generation.load()) { busy=false; progress=-1; status="Background task failed"; error="Not enough memory to complete the task"; }
                }
                catch(const std::exception& e) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(startedGeneration==generation.load()) { busy=false; progress=-1; status="Background task failed"; error=juce::String(e.what()); }
                }
                catch(...) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if(startedGeneration==generation.load()) { busy=false; progress=-1; status="Background task failed"; error="The task could not be completed"; }
                }
                engine.collectRetired();
            }
        });
    }
    ~Impl() {
        cancelled.store(true);
        { std::lock_guard<std::mutex> lock(jobsMutex); stopping = true; jobs.clear(); }
        jobsChanged.notify_all();
        if (worker.joinable()) worker.join();
    }
    void submit(std::function<void()> job, JobKind kind=JobKind::General) {
        { std::lock_guard<std::mutex> lock(jobsMutex); if (stopping) return;
          if(kind==JobKind::Load) jobs.erase(std::remove_if(jobs.begin(),jobs.end(),[](const Job& queued){return queued.kind!=JobKind::General;}),jobs.end());
          jobs.push_back({std::move(job),kind}); }
        jobsChanged.notify_one();
    }
    bool abandonLoad(std::uint64_t expected) {
        if(expected!=generation.load()) return true;
        if(!cancelled.load()) return false;
        std::lock_guard<std::mutex> lock(mutex);
        if(expected==generation.load()) { busy=false; progress=-1; status="Loading cancelled"; }
        return true;
    }
    void setParameter(const juce::String& id, float value) {
        if (auto* parameter = owner.parameters.getParameter(id)) {
            const auto normalised = parameter->convertTo0to1(value);
            if (std::abs(parameter->getValue() - normalised) > 0.0000001f) parameter->setValueNotifyingHost(normalised);
        }
    }
    PlaybackParameters playback() const {
        PlaybackParameters p; p.gainDb=master->load(); p.sourceBpm=sourceBpm->load(); p.targetBpm=targetBpm->load();
        p.semitones=globalSemi->load(); p.cents=globalCents->load(); p.hostMultiplier=multiplier->load();
        p.sync=sync->load()>0.5f; p.hold=hold->load()>0.5f; p.mono=mono->load()>0.5f; p.keyboardMode=keyboard->load()>0.5f; p.rootMidiNote=(int) rootNote->load();
        p.hostBpm=hostBpm.load(); p.selectedPad=selectedPadAtomic.load(); p.repitch=repitch->load()>0.5f; p.velocitySensitive=velocitySensitive->load()>0.5f; p.glideMs=glideMs->load();
        p.metronome=metronome->load()>0.5f;
        for (int s=0;s<stemCount;++s) { p.stemEnabled[(size_t)s]=stemEnabled[(size_t)s]->load()>0.5f; p.stemGainDb[(size_t)s]=stemGain[(size_t)s]->load(); p.stemSolo[(size_t)s]=stemSolo[(size_t)s]->load()>0.5f; }
        for (int i=0;i<padCount;++i) {
            const auto& r=padRefs[(size_t)i]; auto& q=p.pads[(size_t)i];
            q.gainDb=r.gain->load(); q.pan=r.pan->load(); q.semitones=r.semitones->load(); q.cents=r.cents->load(); q.timeRatio=r.time->load();
            q.cutoffHz=r.cutoff->load(); q.resonance=r.resonance->load(); q.attackMs=r.attack->load(); q.releaseMs=r.release->load();
            q.reverse=r.reverse->load()>0.5f; q.loop=r.loop->load()>0.5f; q.output=(int)r.output->load();
        }
        return p;
    }
    std::atomic<int> selectedPadAtomic { 0 };
    SessionState stateWithParameters() const {
        auto state = session; const auto p = playback();
        auto& g=state.global; g.gainDb=p.gainDb; g.sourceBpm=p.sourceBpm; g.targetBpm=p.targetBpm; g.semitones=p.semitones; g.cents=p.cents; g.hostMultiplier=p.hostMultiplier;
        g.sync=p.sync; g.hold=p.hold; g.mono=p.mono; g.keyboardMode=p.keyboardMode; g.rootMidiNote=p.rootMidiNote; g.stemEnabled=p.stemEnabled; g.stemGainDb=p.stemGainDb; g.stemSolo=p.stemSolo; g.repitch=p.repitch; g.velocitySensitive=p.velocitySensitive; g.glideMs=p.glideMs;
        g.metronome=p.metronome;
        for(int i=0;i<padCount;++i) {
            auto& dst=state.pads[(size_t)i]; const auto& src=p.pads[(size_t)i];
            dst.gainDb=src.gainDb; dst.pan=src.pan; dst.semitones=src.semitones; dst.cents=src.cents; dst.timeRatio=src.timeRatio; dst.cutoffHz=src.cutoffHz; dst.resonance=src.resonance; dst.attackMs=src.attackMs; dst.releaseMs=src.releaseMs; dst.reverse=src.reverse; dst.loop=src.loop; dst.output=src.output;
        }
        return state;
    }
    void trimHistory() {
        auto retainedBytes=[&] {
            std::set<const SampleAsset*> seen;
            std::size_t total=0;
            for(const auto* stack:{&previous,&next}) for(const auto& entry:*stack)
                if(entry.asset && entry.asset.get()!=asset.get() && seen.insert(entry.asset.get()).second) total+=pcmBytes(*entry.asset);
            return total;
        };
        while(previous.size()+next.size()>64 || retainedBytes()>maxHistoryBytes) {
            if(!previous.empty()) previous.erase(previous.begin());
            else if(!next.empty()) next.erase(next.begin());
            else break;
        }
    }
    void remember() {
        if (gestureDepth > 0 && gestureRemembered) return;
        if (gestureDepth > 0) gestureRemembered = true;
        previous.push_back({stateWithParameters(), asset, pendingManifest});
        next.clear();
        trimHistory();
    }
    void applyGlobal(const GlobalSettings& g) {
        setParameter("master_gain",g.gainDb); setParameter("source_bpm",(float)g.sourceBpm); setParameter("target_bpm",(float)g.targetBpm);
        setParameter("global_semitones",g.semitones); setParameter("global_cents",g.cents); setParameter("host_multiplier",g.hostMultiplier);
        setParameter("sync",(float)g.sync); setParameter("hold",(float)g.hold); setParameter("mono",(float)g.mono); setParameter("keyboard",(float)g.keyboardMode); setParameter("root_note",(float)g.rootMidiNote); setParameter("repitch",(float)g.repitch); setParameter("velocity_sensitive",(float)g.velocitySensitive); setParameter("glide_ms",g.glideMs);
        setParameter("metronome",(float)g.metronome);
        for(int s=0;s<stemCount;++s) { setParameter(stemId(s,"enabled"),(float)g.stemEnabled[(size_t)s]); setParameter(stemId(s,"gain"),g.stemGainDb[(size_t)s]); setParameter(stemId(s,"solo"),(float)g.stemSolo[(size_t)s]); }
    }
    void applyPad(int i, const PadSettings& p) {
        setParameter(padId(i,"gain"),p.gainDb); setParameter(padId(i,"pan"),p.pan); setParameter(padId(i,"semitones"),p.semitones); setParameter(padId(i,"cents"),p.cents);
        setParameter(padId(i,"time"),p.timeRatio); setParameter(padId(i,"cutoff"),p.cutoffHz); setParameter(padId(i,"resonance"),p.resonance);
        setParameter(padId(i,"attack"),p.attackMs); setParameter(padId(i,"release"),p.releaseMs); setParameter(padId(i,"reverse"),(float)p.reverse); setParameter(padId(i,"loop"),(float)p.loop); setParameter(padId(i,"output"),(float)p.output);
    }
    void publish() { trimHistory(); engine.publish(asset,session); }
    void queueKeyAnalysis(std::shared_ptr<const SampleAsset> source, std::uint64_t expected) {
        submit([this,source=std::move(source),expected] {
            const auto stale=[&] { return cancelled.load() || expected!=generation.load(); };
            const auto detected=externalKey(*source,stale);
            if(detected.confidence.isEmpty() || stale()) return;
            auto updated=std::make_shared<SampleAsset>(*source);
            updated->keyConfidenceLabel="S-KEY " + detected.confidence;
            updated->keyConfidence=0;
            if(detected.confidence=="low") { updated->detectedKey="Unknown"; updated->camelot={}; }
            else { updated->detectedKey=detected.key; updated->camelot=detected.camelot; }
            std::lock_guard<std::mutex> lock(mutex);
            if(stale() || asset!=source) return;
            for(auto* stack:{&previous,&next}) for(auto& entry:*stack) if(entry.asset==source) entry.asset=updated;
            asset=updated; ++session.revision; if(!busy) status="Key analysis ready"; publish();
        },JobKind::Key);
    }
    void restoreEntry(const Entry& e) {
        session=e.state; asset=e.asset; pendingManifest=e.manifest; ++session.revision;
        applyGlobal(session.global); for(int i=0;i<padCount;++i) applyPad(i,session.pads[(size_t)i]); publish();
    }
};

juce::AudioProcessor::BusesProperties SliceLabAudioProcessor::createBuses() {
    juce::AudioProcessor::BusesProperties buses;
    buses = buses.withOutput("Main", juce::AudioChannelSet::stereo(), true);
    for (int i=1;i<padCount;++i) buses = buses.withOutput("Aux " + juce::String(i), juce::AudioChannelSet::stereo(), false);
    return buses;
}
SliceLabAudioProcessor::SliceLabAudioProcessor()
    : AudioProcessor(createBuses()), parameters(*this, nullptr, "PARAMETERS", createParameterLayout()), impl(std::make_unique<Impl>(*this)) {}
SliceLabAudioProcessor::~SliceLabAudioProcessor() = default;
const juce::String SliceLabAudioProcessor::getName() const { return "SliceLab"; }
void SliceLabAudioProcessor::prepareToPlay(double rate, int blockSize) {
    impl->engine.prepareScheduled(rate,blockSize,0);
    setLatencySamples(impl->engine.scheduledLatencySamples());
}
void SliceLabAudioProcessor::releaseResources() { impl->engine.release(); }
bool SliceLabAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
    for (int i=1;i<layouts.outputBuses.size();++i) if (!layouts.getChannelSet(false,i).isDisabled() && layouts.getChannelSet(false,i) != juce::AudioChannelSet::stereo()) return false;
    return layouts.inputBuses.isEmpty();
}
void SliceLabAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    double bpm=0;
    if (auto* playhead=getPlayHead()) if (const auto position=playhead->getPosition()) if (const auto tempo=position->getBpm()) bpm=*tempo;
    if (!std::isfinite(bpm) || bpm < 1 || bpm > 1000) bpm=0;
    impl->hostBpm.store(bpm,std::memory_order_relaxed);
    auto p=impl->playback();
    std::array<int,padCount> offsets {};
    for (int i=1;i<padCount;++i) {
        const auto* bus=getBus(false,i);
        offsets[(size_t)i]=bus && bus->isEnabled() ? getChannelIndexInProcessBlockBuffer(false,i,0) : 0;
    }
    impl->engine.processScheduled(buffer,midi,p,offsets,isNonRealtime()); midi.clear();
}
juce::AudioProcessorEditor* SliceLabAudioProcessor::createEditor() { return new SliceLabAudioProcessorEditor(*this); }
bool SliceLabAudioProcessor::hasEditor() const { return true; }
double SliceLabAudioProcessor::getTailLengthSeconds() const { return 5.0; }
bool SliceLabAudioProcessor::acceptsMidi() const { return true; }
bool SliceLabAudioProcessor::producesMidi() const { return false; }
bool SliceLabAudioProcessor::isMidiEffect() const { return false; }
int SliceLabAudioProcessor::getNumPrograms() { return 1; }
int SliceLabAudioProcessor::getCurrentProgram() { return 0; }
void SliceLabAudioProcessor::setCurrentProgram(int) {}
const juce::String SliceLabAudioProcessor::getProgramName(int) { return {}; }
void SliceLabAudioProcessor::changeProgramName(int,const juce::String&) {}

Snapshot SliceLabAudioProcessor::snapshot() const {
    Snapshot result;
    { std::lock_guard<std::mutex> lock(impl->mutex);
      result.session=impl->stateWithParameters(); result.asset=impl->asset; result.selectedPad=impl->selectedPad;
      result.busy=impl->busy; result.progress=impl->progress; result.status=impl->status; result.error=impl->error;
      result.canUndo=!impl->previous.empty(); result.canRedo=!impl->next.empty(); }
    result.hostBpm=impl->hostBpm.load(); result.playheadSeconds=impl->engine.playheadSeconds(); result.outputPeak=impl->engine.outputPeak();
    result.previewPlaying=impl->engine.previewPlaying(); result.voicesActive=impl->engine.voicesActive();
    if(result.error.isEmpty() && impl->engine.scheduledOverloads()>0) result.error="DSP overload: playback was reset. Retrigger held notes or increase the host buffer.";
    if(result.error.isEmpty() && impl->engine.commandOverflows()>0) result.error="UI trigger queue overflowed; active voices were stopped";
    for (int i=0;i<padCount;++i) result.activity[(size_t)i]=impl->engine.activity(i);
    return result;
}
void SliceLabAudioProcessor::loadSample(const juce::File& file) { loadSampleInternal(file,false); }
void SliceLabAudioProcessor::relinkSample(const juce::File& file) { loadSampleInternal(file,true); }
void SliceLabAudioProcessor::loadSampleInternal(const juce::File& file,bool relinkOnly) {
    juce::String expectedHash, manifest; std::shared_ptr<const SampleAsset> previousAsset;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        if(relinkOnly && impl->session.sourceHash.isEmpty()) { impl->error="This session has no source identity to relink"; return; }
        previousAsset=impl->asset;
        if (relinkOnly || !impl->asset) {
            expectedHash=impl->session.sourceHash;
            manifest=impl->asset && impl->asset->stemsReady ? impl->asset->stemManifestPath : impl->pendingManifest;
        }
        impl->busy=true; impl->progress=-1; impl->status=relinkOnly ? "Relinking sample" : "Loading sample"; impl->error={};
    }
    const auto generation=++impl->generation; impl->cancelled.store(false);
    impl->submit([this,file,generation,expectedHash,manifest,relinkOnly,previousAsset] {
        if(impl->abandonLoad(generation)) return;
        juce::String error; auto asset=decodeSample(file,generation,error);
        if(impl->abandonLoad(generation)) return;
        if(asset && relinkOnly && asset->sourceHash!=expectedHash) { error="Selected file does not match the saved sample"; asset.reset(); }
        if (asset) {
            analyse(*asset);
            if(expectedHash.isNotEmpty() && asset->sourceHash==expectedHash && manifest.isNotEmpty()) {
                auto manifestFile=juce::File(manifest);
                if(!manifestFile.existsAsFile()) manifestFile=file.getParentDirectory().getChildFile(manifestFile.getFileName());
                juce::String stemError;
                if(manifestFile.existsAsFile()) readStemFiles(*asset,manifestFile.getFullPathName(),stemError);
                else stemError="Stem manifest is missing";
                if(stemError.isNotEmpty()) error=stemError;
                if(!asset->stemsReady && previousAsset && previousAsset->stemsReady) {
                    asset->stems=previousAsset->stems; asset->stemPaths=previousAsset->stemPaths;
                    asset->stemsReady=true; asset->stemModel=previousAsset->stemModel; asset->stemManifestPath=previousAsset->stemManifestPath;
                }
            }
        }
        if (generation!=impl->generation.load()) return;
        std::lock_guard<std::mutex> lock(impl->mutex);
        if (impl->cancelled.load()) { impl->busy=false; impl->status="Loading cancelled"; return; }
        if (generation!=impl->generation.load()) return;
        impl->busy=false; impl->progress=-1;
        if (!asset) { impl->status=relinkOnly ? "Relink failed" : "Sample unavailable"; impl->error=error; return; }
        const bool relink = relinkOnly || (!impl->asset && expectedHash.isNotEmpty() && expectedHash==asset->sourceHash);
        impl->remember(); impl->asset=asset; impl->session.sourceHash=asset->sourceHash; impl->session.sourcePath=asset->sourcePath; impl->session.assetGeneration=generation;
        if (!relink) {
            impl->pendingManifest={}; impl->session.beatAnchors.clear();
            impl->session.global.keyOverride=-1;
            for (int i=0;i<padCount;++i) {
                auto pad=PadSettings{}; pad.name="Slice " + juce::String(i+1);
                if (i<16) { pad.startSeconds=asset->durationSeconds()*i/16.0; pad.endSeconds=asset->durationSeconds()*(i+1)/16.0; }
                quantizeBounds(pad,asset->sampleRate,asset->audio.getNumSamples());
                impl->session.pads[(size_t)i]=pad; impl->applyPad(i,pad);
            }
            if (asset->detectedBpm > 0) { impl->session.global.sourceBpm=asset->detectedBpm; impl->setParameter("source_bpm",(float)asset->detectedBpm); }
        }
        if(relink) for(auto& pad:impl->session.pads) quantizeBounds(pad,asset->sampleRate,asset->audio.getNumSamples());
        if(asset->stemsReady) impl->pendingManifest=asset->stemManifestPath;
        ++impl->session.revision; impl->status=relinkOnly ? "Sample relinked" : "Sample ready"; impl->error=error; impl->publish();
        impl->queueKeyAnalysis(asset,generation);
    },Impl::JobKind::Load);
}
void SliceLabAudioProcessor::setGlobal(const GlobalSettings& incoming) {
    const auto global=cleanGlobal(incoming);
    std::lock_guard<std::mutex> lock(impl->mutex); impl->remember(); impl->session.global=global; ++impl->session.revision; impl->applyGlobal(global); impl->publish();
}
bool SliceLabAudioProcessor::setTargetKey(int tonic) {
    if(tonic<0 || tonic>=12) return false;
    std::lock_guard<std::mutex> lock(impl->mutex);
    auto global=impl->stateWithParameters().global;
    const int source=global.keyOverride>=0 ? global.keyOverride : (impl->asset ? keyClass(impl->asset->detectedKey) : -1);
    if(source<0) return false;
    int interval=(tonic-source%12+12)%12;
    if(interval>6) interval-=12;
    impl->remember(); global.semitones=(float)interval; impl->session.global=global; ++impl->session.revision;
    impl->applyGlobal(global); impl->publish();
    return true;
}
void SliceLabAudioProcessor::setPad(int index,const PadSettings& incoming) {
    if (index<0 || index>=padCount) return;
    std::lock_guard<std::mutex> lock(impl->mutex);
    const double duration=impl->asset ? impl->asset->durationSeconds() : std::max({0.0,incoming.startSeconds,incoming.endSeconds});
    auto pad=cleanPad(incoming,duration); if(impl->asset) quantizeBounds(pad,impl->asset->sampleRate,impl->asset->audio.getNumSamples()); impl->remember(); impl->session.pads[(size_t)index]=pad; ++impl->session.revision; impl->applyPad(index,pad); impl->publish();
}
void SliceLabAudioProcessor::selectPad(int index) { if(index<0 || index>=padCount) return; std::lock_guard<std::mutex> lock(impl->mutex); impl->selectedPad=index; impl->selectedPadAtomic.store(index); }
void SliceLabAudioProcessor::triggerPad(int index,float velocity,bool on) { if(index>=0 && index<padCount) impl->engine.enqueueTrigger(index,std::clamp(velocity,0.0f,1.0f),on); }
void SliceLabAudioProcessor::triggerNote(int note,float velocity,bool on) { if(note>=0 && note<=127) impl->engine.enqueueNote(note,std::clamp(velocity,0.0f,1.0f),on); }
void SliceLabAudioProcessor::stopAll() { impl->engine.enqueueStop(); }
void SliceLabAudioProcessor::auditionSource(double seconds) { if(std::isfinite(seconds)) impl->engine.enqueuePreview(std::max(0.0,seconds)); }
void SliceLabAudioProcessor::stopSourcePreview() { impl->engine.enqueueStopPreview(); }
void SliceLabAudioProcessor::autoSlice(SliceMode mode,int count,float sensitivity) {
    std::lock_guard<std::mutex> lock(impl->mutex); if(!impl->asset) return;
    count=std::clamp(count,1,padCount); const double duration=impl->asset->durationSeconds();
    std::vector<double> boundaries { 0.0 };
    if (mode==SliceMode::FindBest) {
        const auto suggestions=suggestBoundaries(*impl->asset,count,sensitivity);
        boundaries.insert(boundaries.end(),suggestions.begin(),suggestions.end());
    } else if (mode==SliceMode::Transients) {
        const auto& transients=impl->asset->transients;
        const int step=std::max(1,(int)std::ceil((double)transients.size()/std::max(1,count-1) * (1.2-0.7*std::clamp((double)sensitivity,0.0,1.0))));
        for (int i=0;i<(int)transients.size() && (int)boundaries.size()<count;i+=step) if(transients[(size_t)i]>0.001 && transients[(size_t)i]<duration-0.001) boundaries.push_back(transients[(size_t)i]);
    } else if(mode==SliceMode::Grid) {
        const auto& anchors=impl->session.beatAnchors;
        const double division=impl->session.global.gridDivisionBeats;
        if(anchors.size()>=2) {
            const double sourceAtZero=-impl->session.global.beatOffsetSeconds;
            auto byTime=std::upper_bound(anchors.begin(),anchors.end(),sourceAtZero,[](double value,const BeatAnchor& a){ return value<a.sourceSeconds; });
            if(byTime==anchors.begin()) ++byTime;
            if(byTime==anchors.end()) --byTime;
            const auto& before=*(byTime-1); const auto& after=*byTime;
            const double startBeat=before.beat+(sourceAtZero-before.sourceSeconds)*(after.beat-before.beat)/(after.sourceSeconds-before.sourceSeconds);
            double beat=std::ceil(startBeat/division)*division;
            for(int tries=0;tries<count+4 && (int)boundaries.size()<count;++tries,beat+=division) {
                auto upper=std::upper_bound(anchors.begin(),anchors.end(),beat,[](double value,const BeatAnchor& a){ return value<a.beat; });
                if(upper==anchors.begin()) ++upper;
                if(upper==anchors.end()) --upper;
                const auto& a=*(upper-1); const auto& b=*upper;
                const double at=a.sourceSeconds+(beat-a.beat)*(b.sourceSeconds-a.sourceSeconds)/(b.beat-a.beat)+impl->session.global.beatOffsetSeconds;
                if(at>=duration) break;
                if(at>0.001) boundaries.push_back(at);
            }
        } else {
            const double step=division*60.0/std::max(20.0,impl->stateWithParameters().global.sourceBpm);
            double at=std::fmod(impl->session.global.beatOffsetSeconds,step);
            if(at<0) at+=step;
            while(at<duration && (int)boundaries.size()<count) { if(at>0.001) boundaries.push_back(at); at+=step; }
        }
    } else {
        juce::Random random((juce::int64)impl->session.randomSeed);
        for(int i=1;i<count;++i) boundaries.push_back(mode==SliceMode::Random ? duration*random.nextDouble() : duration*i/count);
    }
    boundaries.push_back(duration); std::sort(boundaries.begin(),boundaries.end());
    impl->remember();
    int slice=0;
    for(int i=0;i<padCount;++i) {
        auto& pad=impl->session.pads[(size_t)i];
        if(pad.favorite) continue;
        if(slice+1<(int)boundaries.size()) { pad.startSeconds=boundaries[(size_t)slice]; pad.endSeconds=boundaries[(size_t)slice+1]; ++slice; }
        else pad.startSeconds=pad.endSeconds=0;
        quantizeBounds(pad,impl->asset->sampleRate,impl->asset->audio.getNumSamples());
    }
    if(mode==SliceMode::Random) ++impl->session.randomSeed;
    ++impl->session.revision; impl->status="Slices updated"; impl->publish();
}
void SliceLabAudioProcessor::addSlice(double seconds) {
    std::lock_guard<std::mutex> lock(impl->mutex); if(!impl->asset || !std::isfinite(seconds)) return;
    seconds=std::round(seconds*impl->asset->sampleRate)/impl->asset->sampleRate;
    int free=-1, containing=-1;
    for(int i=0;i<padCount;++i) { const auto& p=impl->session.pads[(size_t)i]; if(!p.active() && free<0) free=i; if(seconds>p.startSeconds && seconds<p.endSeconds) containing=i; }
    if(free<0 || containing<0) return; impl->remember();
    auto& original=impl->session.pads[(size_t)containing]; auto copy=original; copy.startSeconds=seconds; original.endSeconds=seconds; copy.name="Slice " + juce::String(free+1);
    impl->session.pads[(size_t)free]=copy; impl->applyPad(free,copy); ++impl->session.revision; impl->publish();
}
void SliceLabAudioProcessor::deletePad(int index) { if(index<0||index>=padCount) return; std::lock_guard<std::mutex> lock(impl->mutex); impl->remember(); auto& p=impl->session.pads[(size_t)index]; p.startSeconds=p.endSeconds=0; ++impl->session.revision; impl->publish(); }
void SliceLabAudioProcessor::duplicatePad(int index) {
    if(index<0||index>=padCount) return; std::lock_guard<std::mutex> lock(impl->mutex);
    for(int i=0;i<padCount;++i) if(!impl->session.pads[(size_t)i].active()) { impl->remember(); auto p=impl->stateWithParameters().pads[(size_t)index]; p.name += " copy"; impl->session.pads[(size_t)i]=p; impl->applyPad(i,p); ++impl->session.revision; impl->publish(); return; }
}
void SliceLabAudioProcessor::moveBoundary(int index,double start,double end) { if(index<0||index>=padCount) return; auto state=snapshot(); auto p=state.session.pads[(size_t)index]; p.startSeconds=start; p.endSeconds=end; setPad(index,p); }
void SliceLabAudioProcessor::undo() { std::lock_guard<std::mutex> lock(impl->mutex); if(impl->previous.empty()) return; impl->next.push_back({impl->stateWithParameters(),impl->asset,impl->pendingManifest}); auto entry=impl->previous.back(); impl->previous.pop_back(); ++impl->generation; impl->cancelled.store(true); impl->busy=false; impl->restoreEntry(entry); }
void SliceLabAudioProcessor::redo() { std::lock_guard<std::mutex> lock(impl->mutex); if(impl->next.empty()) return; impl->previous.push_back({impl->stateWithParameters(),impl->asset,impl->pendingManifest}); auto entry=impl->next.back(); impl->next.pop_back(); ++impl->generation; impl->cancelled.store(true); impl->busy=false; impl->restoreEntry(entry); }

void SliceLabAudioProcessor::getStateInformation(juce::MemoryBlock& block) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    juce::XmlElement root("SliceLabState"); root.setAttribute("schema",1);
    root.setAttribute("source",impl->session.sourcePath); root.setAttribute("sourceHash",impl->session.sourceHash);
    root.setAttribute("selectedPad",impl->selectedPad); root.setAttribute("beatOffset",impl->session.global.beatOffsetSeconds);
    root.setAttribute("gridDivisionBeats",impl->session.global.gridDivisionBeats);
    root.setAttribute("stemManifest",impl->asset && impl->asset->stemsReady ? impl->asset->stemManifestPath : impl->pendingManifest);
    root.setAttribute("randomSeed",juce::String((juce::int64)impl->session.randomSeed));
    root.setAttribute("viewWidth",impl->session.view.width); root.setAttribute("viewHeight",impl->session.view.height);
    root.setAttribute("viewStart",impl->session.view.viewStartSeconds); root.setAttribute("viewEnd",impl->session.view.viewEndSeconds);
    root.setAttribute("selection",juce::String((juce::int64)impl->session.view.selectionMask)); root.setAttribute("follow",impl->session.view.followPlayhead);
    root.setAttribute("repitch",impl->session.global.repitch); root.setAttribute("velocitySensitive",impl->session.global.velocitySensitive);
    root.setAttribute("snap",impl->session.global.snap); root.setAttribute("glide",impl->session.global.glideMs); root.setAttribute("keyOverride",impl->session.global.keyOverride);
    for(int i=0;i<stemCount;++i) root.setAttribute("solo"+juce::String(i),impl->session.global.stemSolo[(size_t)i]);
    auto* pads=root.createNewChildElement("PADS");
    for(int i=0;i<padCount;++i) {
        auto* child=pads->createNewChildElement("PAD"); const auto& p=impl->session.pads[(size_t)i];
        child->setAttribute("index",i); child->setAttribute("start",p.startSeconds); child->setAttribute("end",p.endSeconds);
        child->setAttribute("name",p.name); child->setAttribute("favorite",p.favorite);
    }
    auto* anchors=root.createNewChildElement("ANCHORS");
    for(const auto& anchor:impl->session.beatAnchors) { auto* child=anchors->createNewChildElement("ANCHOR"); child->setAttribute("seconds",anchor.sourceSeconds); child->setAttribute("beat",anchor.beat); }
    root.addChildElement(impl->owner.parameters.copyState().createXml().release());
    copyXmlToBinary(root,block);
}
void SliceLabAudioProcessor::setStateInformation(const void* data,int size) {
    if(!data || size<=0 || size>16*1024*1024) return;
    auto xml=getXmlFromBinary(data,size);
    if(!xml || !xml->hasTagName("SliceLabState") || xml->getIntAttribute("schema")!=1) return;
    auto* pads=xml->getChildByName("PADS"); auto* params=xml->getChildByName("PARAMETERS");
    if(!pads || !params || pads->getNumChildElements()!=padCount) return;
    auto tree=juce::ValueTree::fromXml(*params);
    if(tree.isValid() && tree.getNumChildren()+1==parameters.state.getNumChildren() && !tree.getChildWithProperty("id","metronome").isValid()) {
        juce::ValueTree added("PARAM"); added.setProperty("id","metronome",nullptr); added.setProperty("value",0.0f,nullptr); tree.addChild(added,-1,nullptr);
    }
    if(!tree.isValid() || tree.getNumChildren()!=parameters.state.getNumChildren()) return;
    std::set<juce::String> seen;
    for(const auto child:tree) {
        const auto id=child.getProperty("id").toString(); const auto valueText=child.getProperty("value").toString();
        auto* parameter=parameters.getParameter(id);
        if(!parameter || !seen.insert(id).second || valueText.isEmpty() || !valueText.containsOnly("0123456789+-.eE")) return;
        const double value=valueText.getDoubleValue(); const auto range=parameter->getNormalisableRange();
        if(!std::isfinite(value) || value<range.start-0.001 || value>range.end+0.001) return;
    }
    SessionState state; state.sourcePath=xml->getStringAttribute("source"); state.sourceHash=xml->getStringAttribute("sourceHash");
    if(state.sourcePath.length()>4096 || (state.sourcePath.isNotEmpty() && !juce::File::isAbsolutePath(state.sourcePath)) || (state.sourceHash.isNotEmpty() && (state.sourceHash.length()!=64 || !state.sourceHash.containsOnly("0123456789abcdef")))) return;
    std::array<bool,padCount> used {};
    for(auto* child:pads->getChildIterator()) {
        const int i=child->getIntAttribute("index",-1); const double start=child->getDoubleAttribute("start",-1), end=child->getDoubleAttribute("end",-1);
        if(i<0||i>=padCount||used[(size_t)i]||!std::isfinite(start)||!std::isfinite(end)||start<0||end<start||end>86400) return;
        used[(size_t)i]=true; auto& p=state.pads[(size_t)i]; p.startSeconds=start; p.endSeconds=end; p.name=child->getStringAttribute("name").substring(0,80); p.favorite=child->getBoolAttribute("favorite");
    }
    state.global.beatOffsetSeconds=clampDouble(xml->getDoubleAttribute("beatOffset"),-3600,3600,0);
    state.global.gridDivisionBeats=clampDouble(xml->getDoubleAttribute("gridDivisionBeats",1.0),0.0625,4.0,1.0);
    state.global.repitch=xml->getBoolAttribute("repitch"); state.global.velocitySensitive=xml->getBoolAttribute("velocitySensitive",true);
    state.global.snap=xml->getBoolAttribute("snap",true); state.global.glideMs=clampFloat((float)xml->getDoubleAttribute("glide"),0,2000,0);
    state.global.keyOverride=std::clamp(xml->getIntAttribute("keyOverride",-1),-1,23);
    for(int i=0;i<stemCount;++i) state.global.stemSolo[(size_t)i]=xml->getBoolAttribute("solo"+juce::String(i));
    state.randomSeed=(std::uint32_t)xml->getStringAttribute("randomSeed","1397506370").getLargeIntValue();
    state.view.width=std::clamp(xml->getIntAttribute("viewWidth",1240),640,8192); state.view.height=std::clamp(xml->getIntAttribute("viewHeight",820),400,8192);
    state.view.viewStartSeconds=clampDouble(xml->getDoubleAttribute("viewStart"),0,86400,0); state.view.viewEndSeconds=clampDouble(xml->getDoubleAttribute("viewEnd"),0,86400,0);
    state.view.selectionMask=(std::uint32_t)xml->getStringAttribute("selection","1").getLargeIntValue(); state.view.followPlayhead=xml->getBoolAttribute("follow");
    if(auto* anchors=xml->getChildByName("ANCHORS")) {
        if(anchors->getNumChildElements()>4096) return;
        for(auto* child:anchors->getChildIterator()) {
            const double seconds=child->getDoubleAttribute("seconds",-1),beat=child->getDoubleAttribute("beat",-1);
            if(!std::isfinite(seconds)||!std::isfinite(beat)||seconds<0||seconds>86400||beat<0||beat>1.0e7) return;
            if(!state.beatAnchors.empty()) {
                const auto& last=state.beatAnchors.back();
                if(seconds<=last.sourceSeconds || beat<=last.beat) return;
                const double bpm=60.0*(beat-last.beat)/(seconds-last.sourceSeconds);
                if(!std::isfinite(bpm)||bpm<1||bpm>1000) return;
            }
            state.beatAnchors.push_back({seconds,beat});
        }
    }
    const auto manifest=xml->getStringAttribute("stemManifest");
    if(manifest.length()>4096 || (manifest.isNotEmpty() && !juce::File::isAbsolutePath(manifest))) return;
    if(state.sourcePath.isNotEmpty()) {
        juce::String sizeError;
        try { sizeError=mediaSizeError(juce::File(state.sourcePath),manifest.isNotEmpty()); }
        catch(const std::exception&) { sizeError="Could not inspect the source media"; }
        if(sizeError.isNotEmpty()) { std::lock_guard<std::mutex> lock(impl->mutex); impl->status="Preset not loaded"; impl->error=sizeError; return; }
    }
    const int selected=std::clamp(xml->getIntAttribute("selectedPad"),0,padCount-1);
    const auto generation=++impl->generation; state.assetGeneration=generation; impl->cancelled.store(false);
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        parameters.replaceState(tree); impl->session=state; impl->asset.reset(); impl->pendingManifest=manifest; impl->selectedPad=selected; impl->selectedPadAtomic.store(selected);
        impl->previous.clear(); impl->next.clear(); impl->busy=state.sourcePath.isNotEmpty(); impl->error={}; impl->status=impl->busy ? "Restoring sample" : "Drop a sample to begin"; impl->publish();
    }
    if(state.sourcePath.isEmpty()) return;
    auto completion=std::make_shared<std::promise<void>>(); auto done=completion->get_future();
    impl->submit([this,state,manifest,generation,completion] {
        if(impl->abandonLoad(generation)) { completion->set_value(); return; }
        juce::String error; auto asset=decodeSample(juce::File(state.sourcePath),generation,error);
        if(impl->abandonLoad(generation)) { completion->set_value(); return; }
        if(asset && state.sourceHash.isNotEmpty() && asset->sourceHash!=state.sourceHash) { error="Source file has changed; relink the intended sample"; asset.reset(); }
        if(asset && !state.beatAnchors.empty() && state.beatAnchors.back().sourceSeconds>asset->durationSeconds()) { error="Saved beat grid extends beyond the source"; asset.reset(); }
        if(asset) { analyse(*asset); if(manifest.isNotEmpty()) { juce::String stemError; if(!readStemFiles(*asset,manifest,stemError)) error=stemError; } }
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if(generation==impl->generation.load()) {
                impl->asset=asset;
                if(asset) for(auto& pad:impl->session.pads) quantizeBounds(pad,asset->sampleRate,asset->audio.getNumSamples());
                impl->busy=false; impl->error=error; impl->status=asset ? "Sample restored" : "Sample unavailable"; impl->publish();
            }
        }
        if(asset && generation==impl->generation.load()) impl->queueKeyAnalysis(asset,generation);
        completion->set_value();
    },Impl::JobKind::Load);
    completion.reset();
    done.wait();
}
void SliceLabAudioProcessor::savePreset(const juce::File& file) { juce::MemoryBlock data; getStateInformation(data); if(!file.replaceWithData(data.getData(),data.getSize())) { std::lock_guard<std::mutex> lock(impl->mutex); impl->error="Could not save preset"; } }
void SliceLabAudioProcessor::loadPreset(const juce::File& file) { juce::MemoryBlock data; if(file.loadFileAsData(data)) setStateInformation(data.getData(),(int)data.getSize()); else { std::lock_guard<std::mutex> lock(impl->mutex); impl->error="Could not read preset"; } }

void SliceLabAudioProcessor::separateStems() {
    std::shared_ptr<const SampleAsset> asset;
    { std::lock_guard<std::mutex> lock(impl->mutex); if(!impl->asset || impl->busy) return; asset=impl->asset;
      if((std::size_t)asset->audio.getNumSamples()*2u*sizeof(float)>maxAssetBytes/(stemCount+1u)) { impl->error="Sample is too large to load four stems"; return; }
      impl->busy=true; impl->progress=-1; impl->status="Preparing stem separation"; impl->error={}; }
    impl->cancelled.store(false); const auto generation=impl->generation.load();
    impl->submit([this,asset,generation] {
        const auto overridePath=juce::SystemStats::getEnvironmentVariable("SLICELAB_STEM_WORKER",{});
        juce::File script=overridePath.isNotEmpty() ? juce::File(overridePath) : juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".local/share/slicelab/stem_worker.py");
        if(!script.existsAsFile()) script=juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("scripts/stem_worker.py");
        const auto dir=juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile(".cache/slicelab/results"); dir.createDirectory();
        const auto token=juce::Uuid().toString(); const auto result=dir.getChildFile(token+".result.json"); const auto status=dir.getChildFile(token+".status.json");
        const auto boundResult=dir.getChildFile(token+".bound.json");
        const juce::File source(asset->sourcePath);
        juce::File workerInput=source;
        juce::WavAudioFormat sourceFormat;
        auto sourceStream=source.createInputStream();
        std::unique_ptr<juce::AudioFormatReader> wavSource(sourceStream ? sourceFormat.createReaderFor(sourceStream.release(),true) : nullptr);
        const bool needsCanonical=!wavSource || wavSource->numChannels>2;
        wavSource.reset();
        juce::String error,transportHash=asset->sourceHash;
        const auto cancelled=[this,generation] { return impl->cancelled.load() || generation!=impl->generation.load(); };
        const auto sourceMatches=[&] { return source.existsAsFile() && juce::SHA256(source).toHexString()==asset->sourceHash; };
        if(!script.existsAsFile()) error="Stem worker script is missing";
        else if(!sourceMatches()) error="Source file changed before separation";
        else if(needsCanonical) workerInput=canonicalSeparationInput(*asset,cancelled,transportHash,error);
        if(error.isEmpty() && cancelled()) error="Stem separation cancelled";
        if(error.isEmpty() && !sourceMatches()) error="Source file changed before separation";
        if(error.isEmpty()) {
            std::vector<std::string> args { "python3",script.getFullPathName().toStdString(),"separate","--input",workerInput.getFullPathName().toStdString(),"--result",result.getFullPathName().toStdString(),"--status",status.getFullPathName().toStdString(),"--host","wojtek-ai-pc","--quality","best" };
            std::vector<char*> argv; for(auto& arg:args) argv.push_back(arg.data()); argv.push_back(nullptr);
            posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0); posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);
            pid_t pid=-1; const int started=posix_spawnp(&pid,"python3",&actions,nullptr,argv.data(),environ); posix_spawn_file_actions_destroy(&actions);
            if(started!=0) error="Could not start the stem worker";
            else {
                int processStatus=0; bool interrupted=false;
                while(waitpid(pid,&processStatus,WNOHANG)==0) {
                    if(impl->cancelled.load() || generation!=impl->generation.load()) {
                        interrupted=true; kill(pid,SIGTERM);
                        bool finished=false;
                        for(int i=0;i<20;++i) { if(waitpid(pid,&processStatus,WNOHANG)==pid) { finished=true; break; } std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
                        if(!finished) { kill(pid,SIGKILL); waitpid(pid,&processStatus,0); }
                        break;
                    }
                    const auto info=juce::JSON::parse(status); if(auto* object=info.getDynamicObject()) {
                        const auto detail=object->getProperty("detail").toString();
                        std::lock_guard<std::mutex> lock(impl->mutex);
                        if(generation==impl->generation.load() && detail.isNotEmpty()) { impl->status=detail; const auto value=object->getProperty("progress"); impl->progress=value.isVoid() ? -1.0f : (float)(double)value; }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                if(interrupted) error="Stem separation cancelled";
                else if(!WIFEXITED(processStatus) || WEXITSTATUS(processStatus)!=0) {
                    const auto info=juce::JSON::parse(status); auto* object=info.getDynamicObject();
                    error=object ? object->getProperty("detail").toString() : juce::String("Stem separation failed");
                }
            }
        }
        if(error.isEmpty() && cancelled()) error="Stem separation cancelled";
        if(error.isEmpty() && !sourceMatches()) error="Source file changed during separation";
        if(error.isEmpty() && needsCanonical) bindSeparationManifest(*asset,result,boundResult,transportHash,error);
        std::shared_ptr<SampleAsset> updated;
        if(error.isEmpty()) { updated=copyWithoutStems(*asset); if(!readStemFiles(*updated,(needsCanonical ? boundResult : result).getFullPathName(),error)) updated.reset(); }
        std::lock_guard<std::mutex> lock(impl->mutex);
        if(generation!=impl->generation.load()) return;
        impl->busy=false; impl->progress=-1; impl->error=error;
        if(updated) { impl->asset=updated; impl->pendingManifest=updated->stemManifestPath; impl->status="Four stems ready"; impl->publish(); }
        else impl->status=error.containsIgnoreCase("cancel") ? "Separation cancelled" : "Separation failed";
    });
}
void SliceLabAudioProcessor::cancelWork() { impl->cancelled.store(true); }
void SliceLabAudioProcessor::setBeatAnchors(const std::vector<BeatAnchor>& incoming) {
    if(incoming.size()>4096) return;
    double lastTime=-1,lastBeat=-1;
    for(const auto& a:incoming) {
        if(!std::isfinite(a.sourceSeconds)||!std::isfinite(a.beat)||a.sourceSeconds<0||a.sourceSeconds>86400||a.beat<0||a.beat>1.0e7||a.sourceSeconds<=lastTime||a.beat<=lastBeat) return;
        if(lastTime>=0) { const double bpm=60.0*(a.beat-lastBeat)/(a.sourceSeconds-lastTime); if(!std::isfinite(bpm)||bpm<1||bpm>1000) return; }
        lastTime=a.sourceSeconds; lastBeat=a.beat;
    }
    std::lock_guard<std::mutex> lock(impl->mutex); if(impl->asset && lastTime>impl->asset->durationSeconds()) return;
    impl->remember(); impl->session.beatAnchors=incoming; ++impl->session.revision; impl->publish();
}
void SliceLabAudioProcessor::setViewState(const ViewState& incoming) {
    ViewState view=incoming; view.width=std::clamp(view.width,640,8192); view.height=std::clamp(view.height,400,8192);
    view.viewStartSeconds=clampDouble(view.viewStartSeconds,0,86400,0); view.viewEndSeconds=clampDouble(view.viewEndSeconds,view.viewStartSeconds,86400,view.viewStartSeconds);
    std::lock_guard<std::mutex> lock(impl->mutex); impl->session.view=view; ++impl->session.revision;
}
void SliceLabAudioProcessor::beginEditGesture() { std::lock_guard<std::mutex> lock(impl->mutex); ++impl->gestureDepth; }
void SliceLabAudioProcessor::endEditGesture() { std::lock_guard<std::mutex> lock(impl->mutex); if(impl->gestureDepth>0 && --impl->gestureDepth==0) impl->gestureRemembered=false; }
void SliceLabAudioProcessor::exportPad(int index,const juce::File& file) {
    if(index<0||index>=padCount) return;
    Snapshot shot=snapshot(); if(!shot.asset || !shot.session.pads[(size_t)index].active()) return;
    auto params=impl->playback(); params.hostBpm=shot.hostBpm; params.pads[(size_t)index].loop=false; params.pads[(size_t)index].output=0; params.hold=false;
    impl->submit([this,shot,index,file,params]() mutable {
        SamplerEngine engine; engine.prepare(shot.asset->sampleRate,512); engine.publish(shot.asset,shot.session); engine.enqueueTrigger(index,1.0f,true);
        juce::WavAudioFormat format; auto stream=file.createOutputStream(); std::unique_ptr<juce::AudioFormatWriter> writer;
        if(stream) { stream->setPosition(0); stream->truncate(); auto options=juce::AudioFormatWriterOptions().withSampleRate(shot.asset->sampleRate).withNumChannels(2).withBitsPerSample(24); std::unique_ptr<juce::OutputStream> output(std::move(stream)); writer=format.createWriterFor(output,options); }
        if(!writer) { std::lock_guard<std::mutex> lock(impl->mutex); impl->error="Could not create WAV export"; return; }
        juce::AudioBuffer<float> buffer(2,512); juce::MidiBuffer midi; std::array<int,padCount> offsets {};
        for(int pos=0;pos<100000000;pos+=512) {
            engine.process(buffer,midi,params,offsets);
            if(!writer->writeFromAudioSampleBuffer(buffer,0,buffer.getNumSamples())) { std::lock_guard<std::mutex> lock(impl->mutex); impl->error="Could not write WAV export"; return; }
            if(engine.activity(index)<=0) break;
        }
        std::lock_guard<std::mutex> lock(impl->mutex); impl->status="Pad exported";
    });
}
void SliceLabAudioProcessor::exportMidi(const juce::File& file) {
    const auto state=snapshot().session;
    impl->submit([this,file,state] {
        juce::MidiMessageSequence sequence; int tick=0;
        for(int i=0;i<padCount;++i) if(state.pads[(size_t)i].active()) {
            auto on=juce::MidiMessage::noteOn(1,firstMidiNote+i,(juce::uint8)100); on.setTimeStamp((double)tick); sequence.addEvent(on);
            auto off=juce::MidiMessage::noteOff(1,firstMidiNote+i); off.setTimeStamp((double)tick+960); sequence.addEvent(off); tick+=960;
        }
        sequence.updateMatchedPairs(); juce::MidiFile midi; midi.setTicksPerQuarterNote(960); midi.addTrack(sequence);
        juce::MemoryOutputStream data;
        juce::TemporaryFile temporary(file);
        bool written=false;
        if(midi.writeTo(data)) {
            if(auto output=temporary.getFile().createOutputStream()) {
                written=output->write(data.getData(),data.getDataSize());
                output->flush();
                written=written && output->getStatus().wasOk();
            }
        }
        written=written && temporary.getFile().getSize()==(juce::int64)data.getDataSize();
        if(!written || std::rename(temporary.getFile().getFullPathName().toRawUTF8(),file.getFullPathName().toRawUTF8())!=0) {
            std::lock_guard<std::mutex> lock(impl->mutex); impl->error="Could not export MIDI"; return;
        }
        std::lock_guard<std::mutex> lock(impl->mutex); impl->error={}; impl->status="MIDI exported";
    });
}
juce::Result SliceLabAudioProcessor::importMarkers(const juce::File& file) {
    if(!file.existsAsFile() || file.getSize()>256*1024) return juce::Result::fail("Choose a marker file no larger than 256 KiB");
    std::shared_ptr<const SampleAsset> asset;
    std::uint64_t generation=0;
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        asset=impl->asset; generation=impl->generation.load();
    }
    if(!asset) return juce::Result::fail("Load audio before importing markers");
    auto input=file.createInputStream(); juce::MemoryBlock bytes;
    if(!input || input->readIntoMemoryBlock(bytes,256*1024+1)<=0 || bytes.getSize()>256*1024)
        return juce::Result::fail("Could not read a marker file within the 256 KiB limit");
    const auto* text=static_cast<const char*>(bytes.getData());
    if(std::memchr(text,0,bytes.getSize()) || !juce::CharPointer_UTF8::isValidString(text,(int)bytes.getSize()))
        return juce::Result::fail("The marker file must contain valid UTF-8 text without NUL bytes");
    std::vector<Marker> markers;
    const auto parsed=parseMarkers(juce::String::fromUTF8(text,(int)bytes.getSize()),asset->durationSeconds(),markers);
    if(parsed.failed()) return parsed;
    std::vector<int> frames;
    for(const auto& marker:markers) {
        const auto frame=std::llround(marker.seconds*asset->sampleRate);
        if(frame<0 || frame>=asset->audio.getNumSamples() || (!frames.empty() && frame<=frames.back()))
            return juce::Result::fail("Markers must occupy distinct source frames inside the recording");
        frames.push_back((int)frame);
    }
    std::lock_guard<std::mutex> lock(impl->mutex);
    if(generation!=impl->generation.load() || !impl->asset || impl->asset->sourceHash!=asset->sourceHash)
        return juce::Result::fail("The source changed while markers were being read; retry");
    std::vector<int> slots;
    for(int i=0;i<padCount;++i) if(!impl->session.pads[(size_t)i].favorite) slots.push_back(i);
    if(markers.size()>slots.size()) return juce::Result::fail("There are not enough non-favorite pads for these markers");
    impl->remember();
    for(size_t i=0;i<markers.size();++i) {
        auto& pad=impl->session.pads[(size_t)slots[i]];
        pad.startSeconds=frames[i]/asset->sampleRate;
        pad.endSeconds=(i+1<frames.size() ? frames[i+1] : asset->audio.getNumSamples())/asset->sampleRate;
        pad.name=markers[i].name;
    }
    impl->selectedPad=slots.front(); impl->selectedPadAtomic.store(slots.front());
    impl->session.view.selectionMask=std::uint32_t(1)<<slots.front();
    ++impl->session.revision; impl->status="Imported "+juce::String(markers.size())+" markers"; impl->error={}; impl->publish();
    return juce::Result::ok();
}
void SliceLabAudioProcessor::collectSample(const juce::File& directory) {
    const auto shot=snapshot(); if(!shot.asset) return; const auto generation=impl->generation.load();
    impl->submit([this,shot,directory,generation] {
        auto fail=[this](const juce::String& message) { std::lock_guard<std::mutex> lock(impl->mutex); impl->error=message; };
        const juce::File source(shot.asset->sourcePath);
        if(!source.existsAsFile() || juce::SHA256(source).toHexString()!=shot.asset->sourceHash) { fail("Source file is missing or changed; relink before collecting"); return; }
        juce::var manifest; juce::DynamicObject* object=nullptr; juce::DynamicObject* stems=nullptr;
        std::array<juce::String,stemCount> hashes;
        if(shot.asset->stemsReady) {
            manifest=juce::JSON::parse(juce::File(shot.asset->stemManifestPath)); object=manifest.getDynamicObject();
            if(!object || object->getProperty("input_sha256").toString()!=shot.asset->sourceHash) { fail("Stem manifest is unavailable or mismatched"); return; }
            stems=object->getProperty("stems").getDynamicObject(); auto* files=object->getProperty("files").getDynamicObject();
            if(!stems || !files) { fail("Stem manifest has no verified file checksums"); return; }
            for(int i=0;i<stemCount;++i) {
                auto* item=files->getProperty(stemNames[(size_t)i]).getDynamicObject();
                if(!item) { fail("Stem manifest has no verified file checksums"); return; }
                hashes[(size_t)i]=item->getProperty("sha256").toString(); const juce::File sourceStem(shot.asset->stemPaths[(size_t)i]);
                if(hashes[(size_t)i].length()!=64 || !sourceStem.existsAsFile() || juce::SHA256(sourceStem).toHexString()!=hashes[(size_t)i]) { fail("Stem file is missing or changed; restore stems before collecting"); return; }
            }
        }
        if(!directory.createDirectory().wasOk()) { fail("Could not create media folder"); return; }
        auto copy=std::make_shared<SampleAsset>(*shot.asset); auto dest=directory.getChildFile(source.getFileName());
        if(dest.existsAsFile() && juce::SHA256(dest).toHexString()!=copy->sourceHash) dest=dest.getNonexistentSibling();
        if(source!=dest && !source.copyFileTo(dest)) { fail("Could not collect source media"); return; }
        if(juce::SHA256(dest).toHexString()!=copy->sourceHash) { fail("Collected source failed its checksum"); return; }
        copy->sourcePath=dest.getFullPathName();
        if(copy->stemsReady) {
            for(int i=0;i<stemCount;++i) {
                const juce::File sourceStem(copy->stemPaths[(size_t)i]); auto stemDest=directory.getChildFile(juce::String("stem-")+stemNames[(size_t)i]+".wav");
                if(stemDest.existsAsFile() && juce::SHA256(stemDest).toHexString()!=hashes[(size_t)i]) stemDest=stemDest.getNonexistentSibling();
                if(sourceStem!=stemDest && !sourceStem.copyFileTo(stemDest)) { fail("Could not collect stem media"); return; }
                if(juce::SHA256(stemDest).toHexString()!=hashes[(size_t)i]) { fail("Collected stem failed its checksum"); return; }
                copy->stemPaths[(size_t)i]=stemDest.getFullPathName(); stems->setProperty(stemNames[(size_t)i],stemDest.getFullPathName());
            }
            object->setProperty("source_path",copy->sourcePath); auto manifestDest=directory.getChildFile("slicelab-stems.json").getNonexistentSibling();
            if(!manifestDest.replaceWithText(juce::JSON::toString(manifest))) { fail("Could not write collected stem manifest"); return; }
            copy->stemManifestPath=manifestDest.getFullPathName();
        }
        std::lock_guard<std::mutex> lock(impl->mutex); if(generation!=impl->generation.load()) return;
        impl->remember(); impl->asset=copy; if(copy->stemsReady) impl->pendingManifest=copy->stemManifestPath;
        impl->session.sourcePath=copy->sourcePath; ++impl->session.revision; impl->status="Media collected"; impl->publish();
    });
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new SliceLabAudioProcessor(); }
