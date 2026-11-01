#pragma once

#include "PluginProcessor.h"

class SliceLabAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           public juce::FileDragAndDropTarget,
                                           private juce::Timer {
public:
    explicit SliceLabAudioProcessorEditor(SliceLabAudioProcessor&);
    ~SliceLabAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    bool keyStateChanged(bool isKeyDown) override;
    void focusLost(juce::Component::FocusChangeType) override;
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void filesDropped(const juce::StringArray&, int, int) override;

private:
    void timerCallback() override;
    struct UI;
    SliceLabAudioProcessor& processor;
    std::unique_ptr<UI> ui;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SliceLabAudioProcessorEditor)
};
