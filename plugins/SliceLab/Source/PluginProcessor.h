#pragma once

#include "Domain.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

class SliceLabAudioProcessor final : public juce::AudioProcessor {
public:
    SliceLabAudioProcessor();
    ~SliceLabAudioProcessor() override;

    const juce::String getName() const override;
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;
    double getTailLengthSeconds() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override;
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    slicelab::Snapshot snapshot() const;
    void loadSample(const juce::File&);
    void relinkSample(const juce::File&);
    void setGlobal(const slicelab::GlobalSettings&);
    bool setTargetKey(int tonic);
    void setPad(int, const slicelab::PadSettings&);
    void selectPad(int);
    void triggerPad(int, float velocity, bool noteOn = true);
    void triggerNote(int midiNote, float velocity, bool noteOn = true);
    void stopAll();
    void auditionSource(double sourceSeconds);
    void stopSourcePreview();
    void autoSlice(slicelab::SliceMode, int count, float sensitivity);
    void addSlice(double sourceSeconds);
    void deletePad(int);
    void duplicatePad(int);
    void moveBoundary(int, double startSeconds, double endSeconds);
    void separateStems();
    void cancelWork();
    void setBeatAnchors(const std::vector<slicelab::BeatAnchor>&);
    void setViewState(const slicelab::ViewState&);
    void beginEditGesture();
    void endEditGesture();
    void undo();
    void redo();
    void savePreset(const juce::File&);
    void loadPreset(const juce::File&);
    void exportPad(int, const juce::File&);
    void exportMidi(const juce::File&);
    juce::Result importMarkers(const juce::File&);
    void collectSample(const juce::File& directory);

    juce::AudioProcessorValueTreeState parameters;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    void loadSampleInternal(const juce::File&, bool relinkOnly);
    static BusesProperties createBuses();
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SliceLabAudioProcessor)
};
