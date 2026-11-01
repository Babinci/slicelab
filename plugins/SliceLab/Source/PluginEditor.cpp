#include "PluginEditor.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>

using namespace slicelab;

namespace {
const juce::Colour ink (0xff10151a), panel (0xff192128), raised (0xff222c34), line (0xff36434c);
const juce::Colour lightText (0xffeee9df), muted (0xffa9b5bc), copper (0xfff4a56b);
const std::array<juce::Colour, 4> stemColours { juce::Colour(0xffd4858b), juce::Colour(0xffd6b36e), juce::Colour(0xff79adc0), juce::Colour(0xff8fba9a) };
const std::array<const char*, 4> stemNames { "vocals", "drums", "bass", "other" };
const char* keyboardKeys = "12345678qwertyuiasdfghjkzxcvbnm,";

juce::Font font(float size, bool bold = false) { return juce::Font(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain)); }
juce::String padParameter(int index, const char* property) { return "pad" + juce::String(index + 1).paddedLeft('0', 2) + "_" + property; }
juce::String stemParameter(int index, const char* property) { return "stem_" + juce::String(stemNames[(size_t) index]) + "_" + property; }
const std::array<double,7> gridDivisions { 0.0625,0.125,0.25,0.5,1.0,2.0,4.0 };

int sourceKeyClass(const GlobalSettings& global, const SampleAsset* asset) {
    if(global.keyOverride>=0) return global.keyOverride;
    if(!asset) return -1;
    const auto tonic=asset->detectedKey.upToFirstOccurrenceOf(" ",false,false);
    static const std::array<juce::String,12> sharp { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    static const std::array<juce::String,12> flat { "C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B" };
    for(int i=0;i<12;++i) if(tonic==sharp[(size_t)i] || tonic==flat[(size_t)i]) return i+(asset->detectedKey.containsIgnoreCase("minor")?12:0);
    return -1;
}

bool parseNumber(const juce::String& value, double& result) {
    const auto s = value.trim().toStdString();
    if (s.empty()) return false;
    char* end = nullptr;
    result = std::strtod(s.c_str(), &end);
    return end != s.c_str() && *end == '\0' && std::isfinite(result);
}

double interpolateBeat(const std::vector<BeatAnchor>& anchors, double seconds, double bpm, double offset) {
    if (anchors.size() < 2) return (seconds - offset) * bpm / 60.0;
    seconds -= offset;
    auto it = std::upper_bound(anchors.begin(), anchors.end(), seconds, [](double s, const BeatAnchor& a) { return s < a.sourceSeconds; });
    if (it == anchors.begin()) ++it;
    if (it == anchors.end()) --it;
    const auto& a = *(it - 1); const auto& b = *it;
    return a.beat + (seconds - a.sourceSeconds) * (b.beat - a.beat) / (b.sourceSeconds - a.sourceSeconds);
}

double interpolateTime(const std::vector<BeatAnchor>& anchors, double beat, double bpm, double offset) {
    if (anchors.size() < 2) return offset + beat * 60.0 / std::max(20.0, bpm);
    auto it = std::upper_bound(anchors.begin(), anchors.end(), beat, [](double b, const BeatAnchor& a) { return b < a.beat; });
    if (it == anchors.begin()) ++it;
    if (it == anchors.end()) --it;
    const auto& a = *(it - 1); const auto& b = *it;
    return offset + a.sourceSeconds + (beat - a.beat) * (b.sourceSeconds - a.sourceSeconds) / (b.beat - a.beat);
}

struct StudioLook final : juce::LookAndFeel_V4 {
    StudioLook() {
        setColour(juce::Label::textColourId, lightText);
        setColour(juce::TextEditor::backgroundColourId, raised);
        setColour(juce::TextEditor::textColourId, lightText);
        setColour(juce::TextEditor::outlineColourId, line);
        setColour(juce::TextEditor::focusedOutlineColourId, copper);
        setColour(juce::CaretComponent::caretColourId, copper);
        setColour(juce::PopupMenu::backgroundColourId, panel);
        setColour(juce::PopupMenu::textColourId, lightText);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, raised.brighter(0.2f));
        setColour(juce::PopupMenu::highlightedTextColourId, lightText);
        setColour(juce::ComboBox::backgroundColourId, raised);
        setColour(juce::ComboBox::textColourId, lightText);
        setColour(juce::ComboBox::outlineColourId, line);
        setColour(juce::ComboBox::arrowColourId, muted);
        setColour(juce::Slider::textBoxTextColourId, lightText);
        setColour(juce::Slider::textBoxBackgroundColourId, raised);
        setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour(juce::TooltipWindow::backgroundColourId, juce::Colour(0xff303a40));
        setColour(juce::TooltipWindow::textColourId, lightText);
        setColour(juce::TooltipWindow::outlineColourId, line);
    }
    void drawButtonBackground(juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override {
        auto r = b.getLocalBounds().toFloat().reduced(0.5f);
        auto c = b.getToggleState() ? copper.withAlpha(0.22f) : raised;
        if (!b.isEnabled()) c = panel.brighter(0.02f);
        else if (down) c = c.brighter(0.2f);
        else if (over) c = c.brighter(0.1f);
        g.setColour(c); g.fillRoundedRectangle(r, 4.0f);
        g.setColour(b.getToggleState() ? copper.withAlpha(0.8f) : line); g.drawRoundedRectangle(r, 4.0f, 1.0f);
        if (b.hasKeyboardFocus(true)) { g.setColour(copper); g.drawRoundedRectangle(r.reduced(1.5f), 3.0f, 1.0f); }
    }
    void drawButtonText(juce::Graphics& g, juce::TextButton& b, bool, bool) override {
        g.setFont(font(11.5f, b.getToggleState()));
        g.setColour(b.isEnabled() ? (b.getToggleState() ? copper.brighter(0.15f) : lightText) : muted.withAlpha(0.45f));
        g.drawFittedText(b.getButtonText(), b.getLocalBounds().reduced(3), juce::Justification::centred, 1);
    }
    void drawComboBox(juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override {
        g.setColour(box.isEnabled() ? raised : panel); g.fillRoundedRectangle(0, 0, (float) w, (float) h, 4);
        g.setColour(line); g.drawRoundedRectangle(0.5f, 0.5f, (float) w - 1, (float) h - 1, 4, 1);
        juce::Path p; p.startNewSubPath((float)w - 14, (float)h * 0.42f); p.lineTo((float)w - 10, (float)h * 0.60f); p.lineTo((float)w - 6, (float)h * 0.42f);
        g.setColour(muted); g.strokePath(p, juce::PathStrokeType(1.2f));
    }
    juce::Font getComboBoxFont(juce::ComboBox&) override { return font(11.0f); }
    juce::Font getTextButtonFont(juce::TextButton&, int) override { return font(11.5f); }
    void drawLinearSlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float, float, const juce::Slider::SliderStyle, juce::Slider& slider) override {
        auto r = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h).reduced(0.5f);
        g.setColour(raised); g.fillRoundedRectangle(r, 3);
        const float fill = juce::jlimit(0.0f, r.getWidth(), pos - r.getX());
        g.setColour((slider.isEnabled() ? copper : muted).withAlpha(0.20f)); g.fillRoundedRectangle(r.withWidth(fill), 3);
        g.setColour(line); g.drawRoundedRectangle(r, 3, 1);
    }
    juce::Label* createSliderTextBox(juce::Slider& s) override {
        auto* label = juce::LookAndFeel_V4::createSliderTextBox(s);
        label->setFont(font(11.0f)); label->setJustificationType(juce::Justification::centred);
        return label;
    }
};

struct Numeric final : juce::Component {
    juce::Label label;
    juce::Slider slider { juce::Slider::LinearBar, juce::Slider::TextBoxLeft };
    Numeric(const juce::String& title, double min, double max, double step, double reset, const juce::String& suffix, const juce::String& tip) {
        label.setText(title.toUpperCase(), juce::dontSendNotification); label.setFont(font(9.5f, true)); label.setColour(juce::Label::textColourId, muted); addAndMakeVisible(label);
        slider.setRange(min, max, step); slider.setDoubleClickReturnValue(true, reset); slider.setTextValueSuffix(suffix); slider.setScrollWheelEnabled(true);
        slider.setVelocityModeParameters(0.15, 1, 0.0, true, juce::ModifierKeys::shiftModifier);
        slider.setTooltip(tip + " Drag or use the wheel; Shift-drag adjusts finely. Click to type; double-click resets.");
        slider.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 64, 20);
        slider.valueFromTextFunction = [this, suffix](const juce::String& input) {
            auto value = input.trim();
            if (suffix.isNotEmpty() && value.endsWithIgnoreCase(suffix.trim())) value = value.dropLastCharacters(suffix.trim().length()).trim();
            double parsed = 0;
            return parseNumber(value, parsed) ? juce::jlimit(slider.getMinimum(), slider.getMaximum(), parsed) : slider.getValue();
        };
        addAndMakeVisible(slider);
    }
    void resized() override { auto r = getLocalBounds(); label.setBounds(r.removeFromTop(13)); slider.setBounds(r.reduced(0, 1)); }
};
}

struct SliceLabAudioProcessorEditor::UI {
    struct Waveform;
    struct Pad;
    struct BeatEditor;
    SliceLabAudioProcessorEditor& editor;
    SliceLabAudioProcessor& processor;
    StudioLook look;
    juce::TooltipWindow tooltips { &editor, 650 };
    Snapshot shot;
    ViewState view;
    std::uint64_t assetGeneration = std::numeric_limits<std::uint64_t>::max();
    bool refreshing = false;
    bool gestureOpen = false;
    std::vector<juce::RangedAudioParameter*> gestureParameters;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<Waveform> overview, waveform;
    std::array<std::unique_ptr<Pad>, padCount> pads;
    std::array<bool, padCount> keyDown {};
    std::array<int, padCount> keyNotes {};
    int selectionAnchor = 0;
    double cursor = 0;
    std::vector<double> taps;

    juce::TextButton open { "OPEN" }, file { "FILE" }, undo { "UNDO" }, redo { "REDO" }, panic { "PANIC" };
    juce::TextButton sourceHalf { "/2" }, sourceDouble { "x2" }, targetHalf { "/2" }, targetDouble { "x2" }, tap { "TAP" };
    juce::TextButton sync { "SYNC" }, hold { "HOLD" }, mono { "MONO" }, velocity { "VELOCITY" }, keyboard { "KEYBOARD" }, repitch { "STRETCH" }, snap { "SNAP" }, gridEdit { "GRID EDIT" }, play { "PLAY" }, stop { "STOP" }, follow { "FOLLOW" }, metronome { "METRO" };
    juce::TextButton applySlices { "SLICE" }, add { "+ ADD" }, duplicate { "DUP" }, pitchedDuplicate { "+12 DUP" }, remove { "DELETE" };
    juce::TextButton separate { "SEPARATE" }, cancel { "CANCEL" };
    juce::TextButton reverse { "REVERSE" }, loop { "LOOP" }, favourite { "FAVORITE" };
    juce::ComboBox key, targetKey, sliceMode, gridInterval;
    int sourceKeyModel = -1, targetKeyModel = -1, gridIntervalModel = -1;
    juce::TextEditor name;
    Numeric sourceBpm { "SOURCE BPM", 20, 300, 0.01, 120, "", "Correct the source tempo" };
    Numeric targetBpm { "TARGET BPM", 20, 300, 0.01, 120, "", "Set free playback tempo" };
    Numeric globalSemi { "PITCH", -24, 24, 1, 0, " st", "Transpose all pads" };
    Numeric globalCents { "FINE", -100, 100, 1, 0, " ct", "Fine tune all pads" };
    Numeric master { "MASTER", -60, 12, 0.1, -6, " dB", "Master output gain" };
    Numeric root { "ROOT MIDI", 0, 127, 1, 60, "", "Keyboard mode root MIDI note" };
    Numeric glide { "GLIDE", 0, 2000, 1, 0, " ms", "Keyboard glide time" };
    Numeric offset { "GRID OFFSET", -3600, 3600, 0.001, 0, " s", "Shift the source beat grid" };
    Numeric count { "COUNT", 1, 32, 1, 16, "", "Number of slices" };
    Numeric sensitivity { "SENSITIVITY", 0, 1, 0.01, 0.5, "", "Transient sensitivity" };
    Numeric padStart { "START", 0, 86400, 0.000001, 0, " s", "Slice start in source seconds" };
    Numeric padEnd { "END", 0, 86400, 0.000001, 0, " s", "Slice end in source seconds" };
    Numeric padGain { "GAIN", -60, 12, 0.1, 0, " dB", "Pad gain" };
    Numeric padPan { "PAN", -1, 1, 0.01, 0, "", "Pad stereo pan" };
    Numeric padSemi { "PITCH", -24, 24, 1, 0, " st", "Pad transposition" };
    Numeric padCents { "FINE", -100, 100, 1, 0, " ct", "Pad fine tuning" };
    Numeric padTime { "TIME", 0.25, 4, 0.01, 1, " x", "Pad duration multiplier" };
    Numeric padCutoff { "FILTER", 20, 20000, 1, 20000, " Hz", "Low-pass filter cutoff" };
    Numeric padResonance { "RESONANCE", 0.1, 10, 0.01, 0.7071, "", "Filter resonance" };
    Numeric padAttack { "ATTACK", 0, 2000, 1, 2, " ms", "Attack time" };
    Numeric padRelease { "RELEASE", 0, 5000, 1, 25, " ms", "Release time" };
    Numeric padOutput { "OUTPUT", 0, 31, 1, 0, "", "Stereo output bus. Zero is the main output" };
    std::array<std::unique_ptr<Numeric>, stemCount> stemGain;
    std::array<std::unique_ptr<juce::TextButton>, stemCount> stemMute, stemSolo;
    juce::Rectangle<int> headerBounds, globalBounds, waveBounds, stemBounds, padBounds, inspectorBounds, statusBounds;

    explicit UI(SliceLabAudioProcessorEditor&);
    ~UI();
    void refresh();
    void resized();
    void paint(juce::Graphics&);
    void setView(ViewState);
    void select(int, const juce::ModifierKeys&);
    void begin(const std::vector<juce::String>&);
    void end();
    void editGlobal(const juce::String&, const std::function<void(GlobalSettings&)>&);
    void editPads(const char*, const std::function<void(PadSettings&)>&);
    void configureNumeric(Numeric&, const juce::String&, const std::function<void(double)>&, bool padParam = false);
    void addControl(juce::Component& c) { editor.addAndMakeVisible(c); }
    void openFileMenu();
    void chooseFile(int action);
    void editBeatGrid();
    double snapTime(double seconds, bool bypass) const;
    void sourceScale(double);
    void targetScale(double);
    void duplicatePitch();
    bool keyPressed(const juce::KeyPress&);
    bool keyStateChanged();
    void releaseKeys();
    bool textEditing() const;
};

struct SliceLabAudioProcessorEditor::UI::Pad final : juce::Button {
    UI& ui; int index;
    Pad(UI& owner, int n) : juce::Button("Pad " + juce::String(n + 1)), ui(owner), index(n) {
        setWantsKeyboardFocus(false); setTooltip("Pad " + juce::String(n + 1) + ", MIDI note " + juce::String(firstMidiNote + n) + ", computer key " + juce::String::charToString((juce::juce_wchar)keyboardKeys[n]) + ". Click to audition; Shift or Ctrl selects multiple pads.");
    }
    void paintButton(juce::Graphics& g, bool over, bool down) override {
        auto r = getLocalBounds().toFloat().reduced(1);
        const auto& p = ui.shot.session.pads[(size_t) index];
        const bool selected = (ui.view.selectionMask & (std::uint32_t(1) << index)) != 0;
        auto bg = selected ? juce::Colour(0xff514034) : juce::Colour(0xff253038);
        if (!p.active()) bg = juce::Colour(0xff1b242b);
        if (over || down) bg = bg.brighter(0.14f);
        g.setColour(bg); g.fillRoundedRectangle(r, 5);
        g.setColour(selected ? copper : line); g.drawRoundedRectangle(r, 5, selected ? 1.5f : 1.0f);
        if (ui.shot.activity[(size_t) index] > 0.01f) { g.setColour(copper.withAlpha(0.4f)); g.fillRoundedRectangle(r.reduced(2), 3); }
        g.setColour(p.active() ? lightText : muted.withAlpha(0.6f));
        g.setFont(font(12, true)); g.drawText(juce::String(index + 1).paddedLeft('0', 2), 7, 5, getWidth() - 14, 16, juce::Justification::topLeft);
        g.setFont(font(9.5f)); g.setColour(muted); g.drawText("MIDI " + juce::String(firstMidiNote + index), 7, getHeight() - 17, getWidth() - 14, 12, juce::Justification::centredLeft);
        if (p.favorite) { g.setColour(copper); g.setFont(font(13, true)); g.drawText("*", getWidth() - 18, 4, 12, 16, juce::Justification::centred); }
        if (getHeight() > 53 && p.name.isNotEmpty()) { g.setFont(font(9.5f)); g.setColour(lightText.withAlpha(0.75f)); g.drawFittedText(p.name, 7, 21, getWidth() - 14, 13, juce::Justification::centredLeft, 1); }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        juce::Button::mouseDown(e); ui.editor.grabKeyboardFocus(); ui.select(index, e.mods);
        if (ui.shot.session.pads[(size_t) index].active()) ui.processor.triggerPad(index, 1.0f, true);
    }
    void mouseUp(const juce::MouseEvent& e) override { ui.processor.triggerPad(index, 0, false); juce::Button::mouseUp(e); }
};

struct SliceLabAudioProcessorEditor::UI::Waveform final : juce::Component, juce::SettableTooltipClient {
    UI& ui; bool whole;
    enum class Drag { none, pan, start, end, move } drag = Drag::none;
    double initialMouse = 0, initialStart = 0, initialEnd = 0, initialViewStart = 0, initialViewEnd = 0;
    int editPad = 0;
    Waveform(UI& u, bool isOverview) : ui(u), whole(isOverview) {
        setTooltip(whole ? "Drag to pan the detail view. Wheel zooms around the pointer." : "Click to set the source cursor and select a slice. Double-click to split. Drag handles to trim; Shift-drag a region to move it. Alt bypasses snap. Wheel zooms; drag empty space to pan.");
    }
    double duration() const { return ui.shot.asset ? ui.shot.asset->durationSeconds() : 0.0; }
    double start() const { return whole ? 0 : ui.view.viewStartSeconds; }
    double end() const { return whole ? duration() : ui.view.viewEndSeconds; }
    double timeAt(float x) const { return start() + juce::jlimit(0.0, 1.0, (double)x / std::max(1, getWidth())) * (end() - start()); }
    float xAt(double t) const { return (float)((t - start()) / std::max(1.0e-9, end() - start()) * getWidth()); }
    void paint(juce::Graphics& g) override {
        auto bounds = getLocalBounds().toFloat();
        g.setColour(juce::Colour(0xff111b21)); g.fillRoundedRectangle(bounds, 4);
        if (!ui.shot.asset || end() <= start()) {
            g.setFont(font(whole ? 10 : 17)); g.setColour(muted); g.drawText(whole ? "SOURCE OVERVIEW" : "Drop audio here or choose OPEN", getLocalBounds(), juce::Justification::centred);
            return;
        }
        g.reduceClipRegion(getLocalBounds());
        auto& asset = *ui.shot.asset;
        if (!whole) {
            for (int i = 0; i < padCount; ++i) {
                const auto& p = ui.shot.session.pads[(size_t) i]; if (!p.active() || p.endSeconds < start() || p.startSeconds > end()) continue;
                auto x1 = juce::jlimit(0.0f, (float)getWidth(), xAt(p.startSeconds)); auto x2 = juce::jlimit(0.0f, (float)getWidth(), xAt(p.endSeconds));
                const bool selected = (ui.view.selectionMask & (std::uint32_t(1) << i)) != 0;
                g.setColour((selected ? copper : juce::Colour(0xff79a1aa)).withAlpha(selected ? 0.14f : 0.05f)); g.fillRect(x1, 0.0f, std::max(1.0f, x2 - x1), (float)getHeight());
                g.setColour((selected ? copper : muted).withAlpha(selected ? 0.8f : 0.3f)); g.drawVerticalLine((int)x1, 22, (float)getHeight());
                if (x2 - x1 > 28) { g.setFont(font(9, true)); g.drawText(juce::String(i + 1), (int)x1 + 4, 22, 25, 14, juce::Justification::left); }
            }
            const auto& global = ui.shot.session.global;
            const auto& anchors = ui.shot.session.beatAnchors;
            const double b0 = interpolateBeat(anchors, start(), global.sourceBpm, global.beatOffsetSeconds);
            const double b1 = interpolateBeat(anchors, end(), global.sourceBpm, global.beatOffsetSeconds);
            const double density = std::max(1.0, std::ceil((b1 - b0) / std::max(1, getWidth() / 48)));
            const int step = std::max(1, (int)std::pow(2.0, std::ceil(std::log2(density))));
            for (int beat = (int)std::ceil(b0 / step) * step; beat <= (int)std::floor(b1); beat += step) {
                const auto x = xAt(interpolateTime(anchors, beat, global.sourceBpm, global.beatOffsetSeconds));
                g.setColour(line.withAlpha(beat % 4 == 0 ? 0.8f : 0.35f)); g.drawVerticalLine((int)x, 16, (float)getHeight());
                g.setColour(muted); g.setFont(font(9));
                const int bar = (int)std::floor((double)beat / 4.0) + 1; const int inBar = ((beat % 4) + 4) % 4 + 1;
                g.drawText(juce::String(bar) + "." + juce::String(inBar), (int)x + 3, 2, 45, 13, juce::Justification::left);
                const double outputBpm = global.sync && ui.shot.hostBpm > 1 ? ui.shot.hostBpm * global.hostMultiplier : global.targetBpm;
                if (beat >= 0 && outputBpm > 0) { g.setColour(muted.withAlpha(0.7f)); g.drawText(juce::String(beat * 60.0 / outputBpm, 1) + "s", (int)x + 3, 12, 45, 11, juce::Justification::left); }
            }
            g.setColour(copper.withAlpha(0.34f)); for (double t : asset.transients) if (t >= start() && t <= end()) g.drawVerticalLine((int)xAt(t), (float)getHeight() - 9, (float)getHeight() - 4);
        }
        auto wave = bounds.reduced(0, whole ? 5.0f : 30.0f);
        const auto& peaks = asset.peaks;
        if (!peaks.empty()) {
            g.setColour(whole ? juce::Colour(0xff728d96) : juce::Colour(0xffc0d0ce));
            const float centre = wave.getCentreY(); const float amplitude = wave.getHeight() * 0.46f;
            for (int x = 0; x < getWidth(); ++x) {
                const double a = timeAt((float)x) / duration(), b = timeAt((float)(x + 1)) / duration();
                float lo = 0, hi = 0;
                const int frames=asset.audio.getNumSamples();
                const int firstFrame=juce::jlimit(0,frames-1,(int)(a*frames));
                const int lastFrame=juce::jlimit(firstFrame+1,frames,(int)std::ceil(b*frames));
                if (lastFrame-firstFrame<=96) {
                    const float* left=asset.audio.getReadPointer(0); const float* right=asset.audio.getReadPointer(std::min(1,asset.audio.getNumChannels()-1));
                    for (int i=firstFrame;i<lastFrame;++i) { lo=std::min({lo,left[i],right[i]}); hi=std::max({hi,left[i],right[i]}); }
                } else {
                    const int i0=juce::jlimit(0,(int)peaks.size()-1,(int)(a*peaks.size()));
                    const int i1=juce::jlimit(i0+1,(int)peaks.size(),(int)std::ceil(b*peaks.size()));
                    for (int i=i0;i<i1;++i) { lo=std::min(lo,peaks[(size_t)i].minimum); hi=std::max(hi,peaks[(size_t)i].maximum); }
                }
                g.drawVerticalLine(x, centre - hi * amplitude, centre - lo * amplitude + 1);
            }
        }
        if (whole) {
            auto x1 = xAt(ui.view.viewStartSeconds), x2 = xAt(ui.view.viewEndSeconds);
            g.setColour(copper.withAlpha(0.13f)); g.fillRect(x1, 0.0f, std::max(2.0f, x2 - x1), (float)getHeight());
            g.setColour(copper.withAlpha(0.85f)); g.drawRect(x1, 0.5f, std::max(2.0f, x2 - x1), (float)getHeight() - 1, 1.0f);
        } else {
            const auto& p = ui.shot.session.pads[(size_t)ui.shot.selectedPad];
            if (p.active()) for (double t : { p.startSeconds, p.endSeconds }) if (t >= start() && t <= end()) {
                const float x = xAt(t); g.setColour(copper); g.drawVerticalLine((int)x, 15, (float)getHeight());
                g.fillRoundedRectangle(x - 4, (float)getHeight() * 0.5f - 11, 8, 22, 2);
            }
            g.setColour(muted); g.setFont(font(9));
            for (int tick=0;tick<=4;++tick) {
                const int x=(getWidth()-1)*tick/4; const double t=start()+(end()-start())*tick/4.0;
                g.drawVerticalLine(x,(float)getHeight()-19,(float)getHeight()-15);
                const int labelX=juce::jlimit(3,std::max(3,getWidth()-68),x+3);
                g.drawText(juce::String(t,2)+" s",labelX,getHeight()-15,65,12,juce::Justification::left);
            }
        }
        if (ui.cursor >= start() && ui.cursor <= end()) { g.setColour(juce::Colours::white.withAlpha(0.55f)); g.drawVerticalLine((int)xAt(ui.cursor), 0, (float)getHeight()); }
        if (ui.shot.playheadSeconds >= start() && ui.shot.playheadSeconds <= end()) { g.setColour(juce::Colour(0xffe8d9b9)); g.drawVerticalLine((int)xAt(ui.shot.playheadSeconds), 0, (float)getHeight()); }
    }
    void mouseDown(const juce::MouseEvent& e) override {
        if (!ui.shot.asset) return;
        ui.editor.grabKeyboardFocus();
        initialMouse = timeAt((float)e.x); initialViewStart = ui.view.viewStartSeconds; initialViewEnd = ui.view.viewEndSeconds;
        if (whole) { drag = Drag::pan; panOverview(initialMouse); return; }
        ui.cursor = initialMouse; editPad = juce::jlimit(0, padCount - 1, ui.shot.selectedPad);
        const auto& selected = ui.shot.session.pads[(size_t)editPad]; initialStart = selected.startSeconds; initialEnd = selected.endSeconds;
        if (selected.active() && std::abs(e.x - xAt(selected.startSeconds)) <= 9) drag = Drag::start;
        else if (selected.active() && std::abs(e.x - xAt(selected.endSeconds)) <= 9) drag = Drag::end;
        else if (e.mods.isShiftDown() && selected.active() && initialMouse >= selected.startSeconds && initialMouse <= selected.endSeconds) drag = Drag::move;
        else {
            int found = -1; for (int i = 0; i < padCount; ++i) { const auto& p = ui.shot.session.pads[(size_t)i]; if (p.active() && initialMouse >= p.startSeconds && initialMouse < p.endSeconds) { found = i; break; } }
            if (found >= 0) ui.select(found, e.mods); else drag = Drag::pan;
        }
        if (drag == Drag::start || drag == Drag::end || drag == Drag::move) ui.begin({});
        repaint();
    }
    void panOverview(double centre) {
        const double width = initialViewEnd - initialViewStart, d = duration();
        auto v = ui.view; v.viewStartSeconds = juce::jlimit(0.0, std::max(0.0, d - width), centre - width * 0.5); v.viewEndSeconds = v.viewStartSeconds + width; ui.setView(v);
    }
    void mouseDrag(const juce::MouseEvent& e) override {
        if (!ui.shot.asset || drag == Drag::none) return;
        if (whole) { panOverview(timeAt((float)e.x)); return; }
        const double delta = (double)e.getDistanceFromDragStartX() / std::max(1, getWidth()) * (initialViewEnd - initialViewStart);
        if (drag == Drag::pan) {
            auto v = ui.view; const double width = initialViewEnd - initialViewStart;
            v.viewStartSeconds = juce::jlimit(0.0, std::max(0.0, duration() - width), initialViewStart - delta); v.viewEndSeconds = v.viewStartSeconds + width; ui.setView(v); return;
        }
        double s = initialStart, en = initialEnd;
        const double frame=1.0/ui.shot.asset->sampleRate;
        if (drag == Drag::start) s = juce::jlimit(0.0, std::max(0.0, en - frame), ui.snapTime(initialStart + delta, e.mods.isAltDown()));
        if (drag == Drag::end) en = juce::jlimit(std::min(duration(), s + frame), duration(), ui.snapTime(initialEnd + delta, e.mods.isAltDown()));
        if (drag == Drag::move) { const double length = initialEnd - initialStart; s = juce::jlimit(0.0, std::max(0.0, duration() - length), ui.snapTime(initialStart + delta, e.mods.isAltDown())); en = s + length; }
        ui.processor.moveBoundary(editPad, s, en); ui.refresh();
    }
    void mouseUp(const juce::MouseEvent&) override { if (drag == Drag::start || drag == Drag::end || drag == Drag::move) ui.end(); drag = Drag::none; }
    void mouseDoubleClick(const juce::MouseEvent& e) override { if (!whole && ui.shot.asset) { ui.cursor = ui.snapTime(timeAt((float)e.x), e.mods.isAltDown()); ui.processor.addSlice(ui.cursor); ui.refresh(); } }
    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override {
        if (!ui.shot.asset) return;
        const double d = duration();
        if (d <= 0) return;
        const double before = whole ? timeAt((float)e.x) : timeAt((float)e.x);
        const double oldWidth = ui.view.viewEndSeconds - ui.view.viewStartSeconds;
        const double width = juce::jlimit(std::min(0.05, d), d, oldWidth * std::exp(-wheel.deltaY * 1.5));
        const double relative = whole ? (before - ui.view.viewStartSeconds) / std::max(0.001, oldWidth) : (double)e.x / std::max(1, getWidth());
        auto v = ui.view; v.viewStartSeconds = juce::jlimit(0.0, std::max(0.0, d - width), before - width * juce::jlimit(0.0, 1.0, relative)); v.viewEndSeconds = v.viewStartSeconds + width; ui.setView(v);
    }
};

struct SliceLabAudioProcessorEditor::UI::BeatEditor final : juce::Component {
    juce::Component::SafePointer<SliceLabAudioProcessorEditor> editor;
    juce::Label title, help, error;
    juce::TextEditor anchors;
    juce::TextButton apply { "APPLY" }, cancel { "CANCEL" };
    explicit BeatEditor(SliceLabAudioProcessorEditor& e) : editor(&e) {
        title.setText("Correct the beat grid", juce::dontSendNotification); title.setFont(font(19, true)); addAndMakeVisible(title);
        help.setText("One anchor per line: source seconds, beat number. Both columns must increase. Grid offset shifts the anchor times. Clear the list to use source BPM.", juce::dontSendNotification);
        help.setFont(font(12)); help.setColour(juce::Label::textColourId, muted); addAndMakeVisible(help);
        anchors.setMultiLine(true); anchors.setReturnKeyStartsNewLine(true); anchors.setFont(font(13)); addAndMakeVisible(anchors);
        juce::String existingLines; for (const auto& a : e.processor.snapshot().session.beatAnchors) existingLines += juce::String(a.sourceSeconds, 4) + ", " + juce::String(a.beat, 3) + "\n";
        anchors.setText(existingLines, false);
        error.setColour(juce::Label::textColourId, juce::Colour(0xffff9a8b)); error.setFont(font(11)); addAndMakeVisible(error);
        addAndMakeVisible(apply); addAndMakeVisible(cancel);
        apply.onClick = [this] {
            if (editor == nullptr) return;
            const auto snapshot = editor->processor.snapshot(); const double duration = snapshot.asset ? snapshot.asset->durationSeconds() : 0;
            std::vector<BeatAnchor> parsed; juce::StringArray lines; lines.addLines(anchors.getText());
            for (const auto& lineText : lines) {
                if (lineText.trim().isEmpty()) continue;
                juce::StringArray parts; parts.addTokens(lineText, ", \t", ""); parts.removeEmptyStrings();
                double time = 0, beat = 0;
                if (parts.size() != 2 || !parseNumber(parts[0], time) || !parseNumber(parts[1], beat) || time < 0 || beat < 0 || time > duration || (!parsed.empty() && (time <= parsed.back().sourceSeconds || beat <= parsed.back().beat))) {
                    error.setText("Each row needs an increasing time and beat within the source.", juce::dontSendNotification); return;
                }
                parsed.push_back({ time, beat });
                if (parsed.size()>4096) { error.setText("A grid can contain at most 4096 anchors.", juce::dontSendNotification); return; }
            }
            if (parsed.size() == 1) { error.setText("Use at least two anchors, or clear the list.", juce::dontSendNotification); return; }
            editor->processor.beginEditGesture(); editor->processor.setBeatAnchors(parsed); editor->processor.endEditGesture();
            if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) dialog->exitModalState(1);
        };
        cancel.onClick = [this] { if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>()) dialog->exitModalState(0); };
        setSize(500, 370);
    }
    void paint(juce::Graphics& g) override { g.fillAll(ink); }
    void resized() override {
        auto r = getLocalBounds().reduced(20); title.setBounds(r.removeFromTop(30)); help.setBounds(r.removeFromTop(45));
        auto bottom = r.removeFromBottom(34); cancel.setBounds(bottom.removeFromRight(90)); bottom.removeFromRight(8); apply.setBounds(bottom.removeFromRight(90));
        error.setBounds(r.removeFromBottom(22)); anchors.setBounds(r.reduced(0, 4));
    }
};

SliceLabAudioProcessorEditor::UI::UI(SliceLabAudioProcessorEditor& e) : editor(e), processor(e.processor), shot(processor.snapshot()), view(shot.session.view) {
    editor.setLookAndFeel(&look);
    const std::array<juce::Component*, 41> basic { &open, &file, &undo, &redo, &panic, &sourceHalf, &sourceDouble, &targetHalf, &targetDouble, &tap,
        &sync, &hold, &mono, &velocity, &keyboard, &repitch, &snap, &gridEdit, &play, &stop, &follow, &metronome, &applySlices, &add, &duplicate, &pitchedDuplicate, &remove, &separate, &cancel,
        &reverse, &loop, &favourite, &key, &targetKey, &sliceMode, &gridInterval, &name, &sourceBpm, &targetBpm, &globalSemi, &globalCents };
    for (auto* c : basic) addControl(*c);
    const std::array<Numeric*, 17> more { &master, &root, &glide, &offset, &count, &sensitivity, &padStart, &padEnd, &padGain, &padPan, &padSemi, &padCents, &padTime, &padCutoff, &padResonance, &padAttack, &padRelease };
    for (auto* c : more) addControl(*c);
    addControl(padOutput);
    for (auto* b : { &open, &file, &undo, &redo, &panic, &sourceHalf, &sourceDouble, &targetHalf, &targetDouble, &tap, &sync, &hold, &mono, &velocity, &keyboard, &repitch, &snap, &gridEdit, &play, &stop, &follow, &metronome, &applySlices, &add, &duplicate, &pitchedDuplicate, &remove, &separate, &cancel, &reverse, &loop, &favourite }) b->setWantsKeyboardFocus(false);
    open.setTooltip("Open an audio file"); file.setTooltip("Presets, marker import, exports, collect assets, and relink");
    undo.setTooltip("Undo (Ctrl+Z)"); redo.setTooltip("Redo (Ctrl+Shift+Z)"); panic.setTooltip("Stop all voices");
    sourceHalf.setTooltip("Halve the source BPM correction"); sourceDouble.setTooltip("Double the source BPM correction");
    targetHalf.setTooltip("Halve the target tempo or host multiplier"); targetDouble.setTooltip("Double the target tempo or host multiplier");
    tap.setTooltip("Tap source tempo at least twice"); sync.setTooltip("Follow the host tempo");
    hold.setTooltip("Hold notes until release. Off selects one-shot trigger mode"); mono.setTooltip("Use one voice at a time. Off selects polyphony");
    velocity.setTooltip("Use incoming MIDI velocity"); keyboard.setTooltip("Play the selected slice chromatically from the root note");
    repitch.setTooltip("Switch between independent stretch and coupled repitch"); snap.setTooltip("Snap slice edits to the source beat grid. Hold Alt to bypass");
    gridEdit.setTooltip("Edit source time and beat anchors"); play.setTooltip("Play or pause the source at the waveform cursor"); stop.setTooltip("Stop source audition"); follow.setTooltip("Follow the active playhead in the detail view");
    metronome.setTooltip("Click on the source beat grid during source preview");
    sliceMode.setTooltip("Choose a slicing method"); applySlices.setTooltip("Create slices, preserving favorites"); add.setTooltip("Split at the source cursor");
    duplicate.setTooltip("Copy the selected pad to the first empty slot"); pitchedDuplicate.setTooltip("Copy the selected pad one octave higher"); remove.setTooltip("Clear the selected pad");
    separate.setTooltip("Prepare the four stems on the configured worker"); cancel.setTooltip("Cancel the current background task");
    reverse.setTooltip("Reverse selected pads"); loop.setTooltip("Loop selected pads"); favourite.setTooltip("Protect selected pads when generating slices");
    key.setTooltip("Correct the detected source key, including major or minor mode");
    targetKey.setTooltip("Transpose the global pitch to a target tonic. The source major or minor mode is preserved.");
    gridInterval.setTooltip("Grid slice interval in beats"); gridInterval.setTextWhenNothingSelected("CUSTOM");
    for (auto* b : { &sync, &hold, &mono, &velocity, &keyboard, &snap, &reverse, &loop, &favourite }) b->setClickingTogglesState(false);
    key.addItem("AUTO KEY", 1);
    const std::array<const char*, 12> notes { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    for (int i = 0; i < 24; ++i) key.addItem(juce::String(notes[(size_t)(i % 12)]) + (i < 12 ? " major" : " minor"), i + 2);
    targetKey.setTextWhenNothingSelected("TARGET KEY");
    for(int i=0;i<12;++i) targetKey.addItem("TO " + juce::String(notes[(size_t)i]),i+1);
    const std::array<const char*,7> intervals { "1/16b","1/8b","1/4b","1/2b","1b","2b","4b" };
    for(int i=0;i<(int)intervals.size();++i) gridInterval.addItem(intervals[(size_t)i],i+1);
    sliceMode.addItem("TRANSIENTS", 1); sliceMode.addItem("GRID", 2); sliceMode.addItem("EQUAL", 3); sliceMode.addItem("RANDOM", 4); sliceMode.addItem("SUGGEST", 5); sliceMode.setSelectedId(1, juce::dontSendNotification);
    count.slider.setValue(16, juce::dontSendNotification); sensitivity.slider.setValue(0.5, juce::dontSendNotification);
    name.setMultiLine(false); name.setSelectAllWhenFocused(true); name.setInputRestrictions(80); name.setTooltip("Name for the selected pad"); name.setFont(font(11));
    name.onReturnKey = [this] { name.giveAwayKeyboardFocus(); };
    name.onFocusLost = [this] { if (!refreshing) { auto value = name.getText().trim(); if (value != shot.session.pads[(size_t)shot.selectedPad].name) editPads(nullptr, [value](PadSettings& p) { p.name = value; }); } };
    name.onEscapeKey = [this] { name.setText(shot.session.pads[(size_t)shot.selectedPad].name,false); name.giveAwayKeyboardFocus(); };
    overview = std::make_unique<Waveform>(*this, true); waveform = std::make_unique<Waveform>(*this, false); addControl(*overview); addControl(*waveform);
    for (int i = 0; i < padCount; ++i) { pads[(size_t)i] = std::make_unique<Pad>(*this, i); addControl(*pads[(size_t)i]); }
    for (int i = 0; i < stemCount; ++i) {
        stemGain[(size_t)i] = std::make_unique<Numeric>(juce::String(stemNames[(size_t)i]), -60, 12, 0.1, 0, " dB", "Stem level");
        stemGain[(size_t)i]->label.setColour(juce::Label::textColourId, stemColours[(size_t)i]);
        stemMute[(size_t)i] = std::make_unique<juce::TextButton>("M"); stemSolo[(size_t)i] = std::make_unique<juce::TextButton>("S");
        stemMute[(size_t)i]->setTooltip("Mute " + juce::String(stemNames[(size_t)i]) + ". Solo does not change mute choices."); stemSolo[(size_t)i]->setTooltip("Solo " + juce::String(stemNames[(size_t)i]));
        stemMute[(size_t)i]->setWantsKeyboardFocus(false); stemSolo[(size_t)i]->setWantsKeyboardFocus(false);
        addControl(*stemGain[(size_t)i]); addControl(*stemMute[(size_t)i]); addControl(*stemSolo[(size_t)i]);
        configureNumeric(*stemGain[(size_t)i], stemParameter(i,"gain"), [this,i](double v) { editGlobal({}, [i,v](GlobalSettings& g){ g.stemGainDb[(size_t)i] = (float)v; }); });
        stemMute[(size_t)i]->onClick = [this,i] { editGlobal(stemParameter(i,"enabled"), [i](GlobalSettings& g){ g.stemEnabled[(size_t)i] = !g.stemEnabled[(size_t)i]; }); };
        stemSolo[(size_t)i]->onClick = [this,i] { editGlobal(stemParameter(i,"solo"), [i](GlobalSettings& g){ g.stemSolo[(size_t)i] = !g.stemSolo[(size_t)i]; }); };
    }
    configureNumeric(sourceBpm, "source_bpm", [this](double v){ editGlobal({}, [v](GlobalSettings& g){g.sourceBpm=v;}); });
    configureNumeric(targetBpm, "target_bpm", [this](double v){ editGlobal({}, [v](GlobalSettings& g){g.targetBpm=v;}); });
    configureNumeric(globalSemi, "global_semitones", [this](double v){ editGlobal({}, [v](GlobalSettings& g){g.semitones=(float)v;}); });
    configureNumeric(globalCents, "global_cents", [this](double v){ editGlobal({}, [v](GlobalSettings& g){g.cents=(float)v;}); });
    configureNumeric(master, "master_gain", [this](double v){ editGlobal({}, [v](GlobalSettings& g){g.gainDb=(float)v;}); });
    configureNumeric(root, "root_note", [this](double v){ editGlobal({}, [v](GlobalSettings& g){g.rootMidiNote=(int)v;}); });
    configureNumeric(glide, "glide_ms", [this](double v){ editGlobal({}, [v](GlobalSettings& g){g.glideMs=(float)v;}); });
    configureNumeric(offset, {}, [this](double v){ editGlobal({}, [v](GlobalSettings& g){g.beatOffsetSeconds=v;}); });
    configureNumeric(padStart, {}, [this](double v){editPads(nullptr,[v](PadSettings& p){p.startSeconds=v;});}, true);
    configureNumeric(padEnd, {}, [this](double v){editPads(nullptr,[v](PadSettings& p){p.endSeconds=v;});}, true);
    configureNumeric(padGain, "gain", [this](double v){editPads(nullptr,[v](PadSettings& p){p.gainDb=(float)v;});}, true);
    configureNumeric(padPan, "pan", [this](double v){editPads(nullptr,[v](PadSettings& p){p.pan=(float)v;});}, true);
    configureNumeric(padSemi, "semitones", [this](double v){editPads(nullptr,[v](PadSettings& p){p.semitones=(float)v;});}, true);
    configureNumeric(padCents, "cents", [this](double v){editPads(nullptr,[v](PadSettings& p){p.cents=(float)v;});}, true);
    configureNumeric(padTime, "time", [this](double v){editPads(nullptr,[v](PadSettings& p){p.timeRatio=(float)v;});}, true);
    configureNumeric(padCutoff, "cutoff", [this](double v){editPads(nullptr,[v](PadSettings& p){p.cutoffHz=(float)v;});}, true);
    configureNumeric(padResonance, "resonance", [this](double v){editPads(nullptr,[v](PadSettings& p){p.resonance=(float)v;});}, true);
    configureNumeric(padAttack, "attack", [this](double v){editPads(nullptr,[v](PadSettings& p){p.attackMs=(float)v;});}, true);
    configureNumeric(padRelease, "release", [this](double v){editPads(nullptr,[v](PadSettings& p){p.releaseMs=(float)v;});}, true);
    configureNumeric(padOutput, "output", [this](double v){editPads(nullptr,[v](PadSettings& p){p.output=(int)v;});}, true);
    open.onClick = [this]{chooseFile(0);}; file.onClick = [this]{openFileMenu();}; undo.onClick = [this]{processor.undo();refresh();}; redo.onClick = [this]{processor.redo();refresh();}; panic.onClick = [this]{processor.stopAll();processor.stopSourcePreview();};
    sourceHalf.onClick = [this]{sourceScale(0.5);}; sourceDouble.onClick = [this]{sourceScale(2);}; targetHalf.onClick = [this]{targetScale(0.5);}; targetDouble.onClick = [this]{targetScale(2);};
    tap.onClick = [this] { const double now = juce::Time::getMillisecondCounterHiRes() / 1000.0; if (!taps.empty() && now - taps.back() > 3.0) taps.clear(); taps.push_back(now); if (taps.size() > 5) taps.erase(taps.begin()); if (taps.size() >= 2) { const double bpm = 60.0 * (taps.size()-1) / (taps.back()-taps.front()); editGlobal("source_bpm", [bpm](GlobalSettings& g){g.sourceBpm=bpm;}); } };
    sync.onClick = [this]{editGlobal("sync",[](GlobalSettings& g){g.sync=!g.sync;});};
    hold.onClick = [this]{editGlobal("hold",[](GlobalSettings& g){g.hold=!g.hold;});};
    mono.onClick = [this]{editGlobal("mono",[](GlobalSettings& g){g.mono=!g.mono;});};
    velocity.onClick = [this]{editGlobal("velocity_sensitive",[](GlobalSettings& g){g.velocitySensitive=!g.velocitySensitive;});};
    keyboard.onClick = [this]{editGlobal("keyboard",[](GlobalSettings& g){g.keyboardMode=!g.keyboardMode;});};
    repitch.onClick = [this]{editGlobal("repitch",[](GlobalSettings& g){g.repitch=!g.repitch;});};
    snap.onClick = [this]{editGlobal({},[](GlobalSettings& g){g.snap=!g.snap;});};
    key.onChange = [this]{if(!refreshing) editGlobal({},[this](GlobalSettings& g){g.keyOverride=key.getSelectedId()-2;});};
    targetKey.onChange = [this]{if(!refreshing && targetKey.getSelectedId()>0) { begin({"global_semitones"}); processor.setTargetKey(targetKey.getSelectedId()-1); end(); refresh(); }};
    sliceMode.onChange = [this]{ if(!refreshing) refresh(); };
    gridInterval.onChange = [this]{const int id=gridInterval.getSelectedId(); if(!refreshing && id>0) editGlobal({},[id](GlobalSettings& g){g.gridDivisionBeats=gridDivisions[(size_t)id-1];});};
    gridEdit.onClick = [this]{editBeatGrid();};
    play.onClick = [this]{if(shot.previewPlaying) {cursor=shot.playheadSeconds; processor.stopSourcePreview();} else processor.auditionSource(cursor);};
    stop.onClick = [this]{processor.stopSourcePreview();};
    follow.onClick = [this]{auto v=view; v.followPlayhead=!v.followPlayhead; setView(v); refresh();};
    metronome.onClick = [this]{editGlobal("metronome",[](GlobalSettings& g){g.metronome=!g.metronome;});};
    applySlices.onClick = [this]{processor.autoSlice((SliceMode)juce::jlimit(0,4,sliceMode.getSelectedId()-1),(int)count.slider.getValue(),(float)sensitivity.slider.getValue());refresh();};
    add.onClick = [this]{processor.addSlice(snapTime(cursor,false));refresh();}; duplicate.onClick = [this]{processor.duplicatePad(shot.selectedPad);refresh();}; pitchedDuplicate.onClick = [this]{duplicatePitch();}; remove.onClick = [this]{processor.deletePad(shot.selectedPad);refresh();};
    reverse.onClick = [this]{const bool v=!shot.session.pads[(size_t)shot.selectedPad].reverse;editPads("reverse",[v](PadSettings& p){p.reverse=v;});};
    loop.onClick = [this]{const bool v=!shot.session.pads[(size_t)shot.selectedPad].loop;editPads("loop",[v](PadSettings& p){p.loop=v;});};
    favourite.onClick = [this]{const bool v=!shot.session.pads[(size_t)shot.selectedPad].favorite;editPads(nullptr,[v](PadSettings& p){p.favorite=v;});};
    separate.onClick = [this]{processor.separateStems();}; cancel.onClick = [this]{processor.cancelWork();};
    refresh();
}

SliceLabAudioProcessorEditor::UI::~UI() {
    end();
    releaseKeys();
    editor.setLookAndFeel(nullptr);
}

void SliceLabAudioProcessorEditor::UI::begin(const std::vector<juce::String>& ids) {
    if (gestureOpen) return;
    gestureOpen = true; processor.beginEditGesture(); gestureParameters.clear();
    for (const auto& id : ids) if (id.isNotEmpty()) if (auto* parameter = processor.parameters.getParameter(id)) {
        if (std::find(gestureParameters.begin(), gestureParameters.end(), parameter) == gestureParameters.end()) { parameter->beginChangeGesture(); gestureParameters.push_back(parameter); }
    }
}
void SliceLabAudioProcessorEditor::UI::end() {
    if (!gestureOpen) return;
    for (auto* p : gestureParameters) p->endChangeGesture();
    gestureParameters.clear(); processor.endEditGesture(); gestureOpen = false;
}
void SliceLabAudioProcessorEditor::UI::editGlobal(const juce::String& id, const std::function<void(GlobalSettings&)>& fn) {
    const bool own = !gestureOpen; if (own) begin({ id });
    auto g = processor.snapshot().session.global; fn(g); processor.setGlobal(g);
    if (own) end();
    refresh();
}
void SliceLabAudioProcessorEditor::UI::editPads(const char* property, const std::function<void(PadSettings&)>& fn) {
    const bool own = !gestureOpen;
    if (own) { std::vector<juce::String> ids; if (property != nullptr) for(int i=0;i<padCount;++i) if(view.selectionMask & (std::uint32_t(1)<<i)) ids.push_back(padParameter(i,property)); begin(ids); }
    const auto state = processor.snapshot().session;
    for (int i=0;i<padCount;++i) if (view.selectionMask & (std::uint32_t(1)<<i)) { auto p = state.pads[(size_t)i]; fn(p); processor.setPad(i,p); }
    if (own) end();
    refresh();
}
void SliceLabAudioProcessorEditor::UI::configureNumeric(Numeric& n, const juce::String& parameter, const std::function<void(double)>& fn, bool padParam) {
    auto ids = [this,parameter,padParam] {
        std::vector<juce::String> result;
        if (parameter.isNotEmpty()) {
            if (padParam) for (int i=0;i<padCount;++i) { if (view.selectionMask & (std::uint32_t(1)<<i)) result.push_back(padParameter(i,parameter.toRawUTF8())); }
            else result.push_back(parameter);
        }
        return result;
    };
    n.slider.onDragStart = [this,ids]{begin(ids());};
    n.slider.onDragEnd = [this]{end();};
    n.slider.onValueChange = [this,&n,fn,ids] { if (refreshing) return; const bool own = !gestureOpen; if (own) begin(ids()); fn(n.slider.getValue()); if (own) end(); };
}
void SliceLabAudioProcessorEditor::UI::setView(ViewState next) {
    const double duration = shot.asset ? shot.asset->durationSeconds() : 0;
    if (duration > 0) {
        next.viewStartSeconds = juce::jlimit(0.0, duration, next.viewStartSeconds);
        next.viewEndSeconds = juce::jlimit(next.viewStartSeconds, duration, next.viewEndSeconds);
    }
    next.width = editor.getWidth(); next.height = editor.getHeight(); if (next.selectionMask == 0) next.selectionMask = 1;
    view = next; processor.setViewState(view); if (overview) overview->repaint(); if (waveform) waveform->repaint();
    for (auto& p : pads) if (p) p->repaint();
}
void SliceLabAudioProcessorEditor::UI::select(int index, const juce::ModifierKeys& mods) {
    if (index < 0 || index >= padCount) return;
    auto v = view; const auto bit = std::uint32_t(1) << index;
    if (mods.isShiftDown()) {
        const int low = std::min(index, selectionAnchor), high = std::max(index, selectionAnchor);
        std::uint32_t mask = 0; for (int i=low;i<=high;++i) mask |= std::uint32_t(1)<<i;
        v.selectionMask = mods.isCtrlDown() || mods.isCommandDown() ? (v.selectionMask | mask) : mask;
    } else if (mods.isCtrlDown() || mods.isCommandDown()) {
        v.selectionMask ^= bit; if (v.selectionMask == 0) v.selectionMask = bit; selectionAnchor = index;
    } else { v.selectionMask = bit; selectionAnchor = index; }
    if ((v.selectionMask & bit) == 0) for (int i=0;i<padCount;++i) if (v.selectionMask & (std::uint32_t(1)<<i)) { index=i; break; }
    processor.selectPad(index); setView(v); refresh();
}
double SliceLabAudioProcessorEditor::UI::snapTime(double seconds, bool bypass) const {
    if (!shot.asset) return seconds;
    const auto& g = shot.session.global;
    if (g.snap && !bypass) { const double beat = interpolateBeat(shot.session.beatAnchors,seconds,g.sourceBpm,g.beatOffsetSeconds); seconds = interpolateTime(shot.session.beatAnchors,std::round(beat),g.sourceBpm,g.beatOffsetSeconds); }
    return juce::jlimit(0.0, shot.asset->durationSeconds(), seconds);
}
void SliceLabAudioProcessorEditor::UI::sourceScale(double factor) { editGlobal("source_bpm",[factor](GlobalSettings& g){g.sourceBpm=juce::jlimit(20.0,300.0,g.sourceBpm*factor);}); }
void SliceLabAudioProcessorEditor::UI::targetScale(double factor) {
    if (processor.snapshot().session.global.sync) editGlobal("host_multiplier",[factor](GlobalSettings& g){g.hostMultiplier=(float)juce::jlimit(0.25,4.0,g.hostMultiplier*factor);});
    else editGlobal("target_bpm",[factor](GlobalSettings& g){g.targetBpm=juce::jlimit(20.0,300.0,g.targetBpm*factor);});
}
void SliceLabAudioProcessorEditor::UI::duplicatePitch() {
    const auto state=processor.snapshot(); int slot=-1;
    for(int i=0;i<padCount;++i) if(!state.session.pads[(size_t)i].active()) { slot=i; break; }
    if(slot<0 || !state.session.pads[(size_t)state.selectedPad].active()) return;
    begin({padParameter(slot,"semitones")}); processor.duplicatePad(state.selectedPad);
    auto p=processor.snapshot().session.pads[(size_t)slot]; p.semitones=juce::jlimit(-24.0f,24.0f,p.semitones+12.0f); processor.setPad(slot,p); end(); refresh();
}
void SliceLabAudioProcessorEditor::UI::refresh() {
    shot = processor.snapshot();
    if (shot.asset && shot.asset->generation != assetGeneration) {
        assetGeneration=shot.asset->generation; view=shot.session.view;
        const double d=shot.asset->durationSeconds();
        if (view.viewEndSeconds<=view.viewStartSeconds || view.viewStartSeconds>=d) { view.viewStartSeconds=0; view.viewEndSeconds=d; }
        else { view.viewStartSeconds=juce::jlimit(0.0,d,view.viewStartSeconds); view.viewEndSeconds=juce::jlimit(view.viewStartSeconds,d,view.viewEndSeconds); }
        cursor=juce::jlimit(0.0,d,cursor); setView(view);
    } else if (!gestureOpen) view = shot.session.view;
    if (view.selectionMask==0) view.selectionMask=std::uint32_t(1)<<juce::jlimit(0,padCount-1,shot.selectedPad);
    if (view.followPlayhead && shot.asset && (shot.previewPlaying || shot.voicesActive) && !waveform->isMouseButtonDown()) {
        const double width=view.viewEndSeconds-view.viewStartSeconds, d=shot.asset->durationSeconds();
        if (width>0 && (shot.playheadSeconds<view.viewStartSeconds || shot.playheadSeconds>view.viewEndSeconds)) {
            auto next=view; next.viewStartSeconds=juce::jlimit(0.0,std::max(0.0,d-width),shot.playheadSeconds-width*0.25); next.viewEndSeconds=next.viewStartSeconds+width; setView(next);
        }
    }
    refreshing=true;
    const auto& g=shot.session.global; const auto& p=shot.session.pads[(size_t)juce::jlimit(0,padCount-1,shot.selectedPad)];
    auto set = [](Numeric& n,double v){ if (!n.slider.isMouseButtonDown() && !n.slider.hasKeyboardFocus(true)) n.slider.setValue(v,juce::dontSendNotification); };
    set(sourceBpm,g.sourceBpm); set(targetBpm,g.targetBpm); set(globalSemi,g.semitones); set(globalCents,g.cents); set(master,g.gainDb); set(root,g.rootMidiNote); set(glide,g.glideMs); set(offset,g.beatOffsetSeconds);
    set(padStart,p.startSeconds); set(padEnd,p.endSeconds); set(padGain,p.gainDb); set(padPan,p.pan); set(padSemi,p.semitones); set(padCents,p.cents); set(padTime,p.timeRatio); set(padCutoff,p.cutoffHz); set(padResonance,p.resonance); set(padAttack,p.attackMs); set(padRelease,p.releaseMs); set(padOutput,p.output);
    if (!name.hasKeyboardFocus(true)) name.setText(p.name,false);
    sync.setToggleState(g.sync,juce::dontSendNotification); hold.setToggleState(g.hold,juce::dontSendNotification); mono.setToggleState(g.mono,juce::dontSendNotification);
    velocity.setToggleState(g.velocitySensitive,juce::dontSendNotification); keyboard.setToggleState(g.keyboardMode,juce::dontSendNotification); snap.setToggleState(g.snap,juce::dontSendNotification);
    reverse.setToggleState(p.reverse,juce::dontSendNotification); loop.setToggleState(p.loop,juce::dontSendNotification); favourite.setToggleState(p.favorite,juce::dontSendNotification);
    repitch.setButtonText(g.repitch ? "REPITCH" : "STRETCH"); repitch.setToggleState(g.repitch,juce::dontSendNotification);
    follow.setToggleState(view.followPlayhead,juce::dontSendNotification); metronome.setToggleState(g.metronome,juce::dontSendNotification); play.setButtonText(shot.previewPlaying ? "PAUSE" : "PLAY");
    auto syncChoice = [](juce::ComboBox& control, int value, int& model) {
        if (model == value) return;
        model = value;
        control.setSelectedId(value, juce::dontSendNotification);
    };
    syncChoice(key, g.keyOverride+2, sourceKeyModel);
    const int sourceKey=sourceKeyClass(g,shot.asset.get());
    syncChoice(targetKey, sourceKey>=0 ? ((sourceKey%12+(int)std::lround(g.semitones))%12+12)%12+1 : 0, targetKeyModel);
    int intervalId=0; for(int i=0;i<(int)gridDivisions.size();++i) if(std::abs(gridDivisions[(size_t)i]-g.gridDivisionBeats)<1.0e-9) intervalId=i+1;
    syncChoice(gridInterval, intervalId, gridIntervalModel);
    for(int i=0;i<stemCount;++i) { set(*stemGain[(size_t)i],g.stemGainDb[(size_t)i]); stemMute[(size_t)i]->setToggleState(!g.stemEnabled[(size_t)i],juce::dontSendNotification); stemSolo[(size_t)i]->setToggleState(g.stemSolo[(size_t)i],juce::dontSendNotification); }
    undo.setEnabled(shot.canUndo); redo.setEnabled(shot.canRedo); cancel.setEnabled(shot.busy);
    const bool hasAsset=shot.asset!=nullptr; const bool ready=hasAsset && shot.asset->stemsReady;
    targetKey.setEnabled(hasAsset && sourceKey>=0); gridInterval.setEnabled(hasAsset && sliceMode.getSelectedId()==2);
    sensitivity.setEnabled(hasAsset && (sliceMode.getSelectedId()==1 || sliceMode.getSelectedId()==5));
    separate.setButtonText(ready?"RESEPARATE":(shot.error.isNotEmpty()?"RETRY":"SEPARATE")); separate.setEnabled(hasAsset && !shot.busy);
    for(int i=0;i<stemCount;++i) { stemGain[(size_t)i]->setEnabled(ready); stemMute[(size_t)i]->setEnabled(ready); stemSolo[(size_t)i]->setEnabled(ready); }
    for(auto* b : {&play,&stop,&applySlices,&add,&duplicate,&remove,&gridEdit}) b->setEnabled(hasAsset);
    bool hasEmpty=false; for(const auto& pad:shot.session.pads) if(!pad.active()) {hasEmpty=true; break;}
    pitchedDuplicate.setEnabled(hasAsset && hasEmpty && p.active() && p.semitones<=12.0f);
    refreshing=false; editor.repaint(); overview->repaint(); waveform->repaint(); for(auto& pad:pads) pad->repaint();
}

void SliceLabAudioProcessorEditor::UI::resized() {
    auto all=editor.getLocalBounds().reduced(10); headerBounds=all.removeFromTop(38); all.removeFromTop(5); globalBounds=all.removeFromTop(83); all.removeFromTop(5);
    statusBounds=all.removeFromBottom(22); all.removeFromBottom(4);
    const int contentHeight=juce::jlimit(205,300,(int)(editor.getHeight()*0.305));
    auto content=all.removeFromBottom(contentHeight); all.removeFromBottom(5); stemBounds=all.removeFromBottom(48); all.removeFromBottom(5);
    auto toolbar=all.removeFromBottom(39); all.removeFromBottom(5); waveBounds=all;
    auto h=headerBounds; auto right=h.removeFromRight(290); panic.setBounds(right.removeFromRight(58).reduced(2,5)); redo.setBounds(right.removeFromRight(55).reduced(2,5)); undo.setBounds(right.removeFromRight(55).reduced(2,5)); file.setBounds(right.removeFromRight(52).reduced(2,5)); open.setBounds(right.removeFromRight(65).reduced(2,5));
    auto rows=globalBounds.reduced(6,1); auto top=rows.removeFromTop(40); rows.removeFromTop(2); auto bottom=rows;
    auto put=[&](juce::Component& c,juce::Rectangle<int>& r,int w,int y=0,int height=0){auto x=r.removeFromLeft(w); r.removeFromLeft(4); if(height>0) x=x.withY(x.getY()+y).withHeight(height); c.setBounds(x);};
    put(sourceBpm,top,71); put(sourceHalf,top,25,13,24); put(sourceDouble,top,25,13,24); put(tap,top,34,13,24); put(targetBpm,top,71); put(targetHalf,top,25,13,24); put(targetDouble,top,25,13,24); put(sync,top,50,13,24); put(globalSemi,top,59); put(globalCents,top,59); put(key,top,100,13,24); put(targetKey,top,79,13,24); put(repitch,top,75,13,24); put(master,top,67);
    put(hold,bottom,59,12,24); put(mono,bottom,59,12,24); put(velocity,bottom,72,12,24); put(keyboard,bottom,75,12,24); put(root,bottom,66); put(glide,bottom,65); put(offset,bottom,86); put(snap,bottom,50,12,24); put(gridEdit,bottom,76,12,24); put(play,bottom,50,12,24); put(stop,bottom,50,12,24); put(follow,bottom,62,12,24); put(metronome,bottom,53,12,24);
    auto w=waveBounds.reduced(4); overview->setBounds(w.removeFromTop(27)); w.removeFromTop(4); waveform->setBounds(w);
    auto t=toolbar.reduced(2,0); put(sliceMode,t,108,13,24); put(gridInterval,t,70,13,24); put(count,t,57); put(sensitivity,t,76); put(applySlices,t,52,13,24); put(add,t,60,13,24); put(duplicate,t,49,13,24); put(pitchedDuplicate,t,65,13,24); put(remove,t,58,13,24);
    auto separateArea=t.removeFromRight(158); cancel.setBounds(separateArea.removeFromRight(58).withTrimmedTop(13).withHeight(24)); separateArea.removeFromRight(4); separate.setBounds(separateArea.withTrimmedTop(13).withHeight(24));
    auto stems=stemBounds.reduced(4,2); const int stemWidth=stems.getWidth()/4;
    for(int i=0;i<stemCount;++i) { auto s=stems.removeFromLeft(i==3?stems.getWidth():stemWidth).reduced(3,0); stemSolo[(size_t)i]->setBounds(s.removeFromRight(28).withTrimmedTop(15).withHeight(24)); s.removeFromRight(3); stemMute[(size_t)i]->setBounds(s.removeFromRight(28).withTrimmedTop(15).withHeight(24)); s.removeFromRight(4); stemGain[(size_t)i]->setBounds(s); }
    padBounds=content.removeFromLeft((int)(content.getWidth()*0.57)); content.removeFromLeft(6); inspectorBounds=content;
    auto grid=padBounds.reduced(4); grid.removeFromTop(18); const int cellW=grid.getWidth()/8, cellH=grid.getHeight()/4;
    for(int row=0;row<4;++row) for(int col=0;col<8;++col) { const int i=row*8+col; pads[(size_t)i]->setBounds(grid.getX()+col*cellW,grid.getY()+row*cellH,col==7?grid.getRight()-(grid.getX()+col*cellW):cellW,row==3?grid.getBottom()-(grid.getY()+row*cellH):cellH); }
    auto inspector=inspectorBounds.reduced(7,4); inspector.removeFromTop(17); auto nameRow=inspector.removeFromTop(25); name.setBounds(nameRow.removeFromLeft(juce::jmax(80,nameRow.getWidth()-166))); nameRow.removeFromLeft(4); reverse.setBounds(nameRow.removeFromLeft(53)); nameRow.removeFromLeft(3); loop.setBounds(nameRow.removeFromLeft(47)); nameRow.removeFromLeft(3); favourite.setBounds(nameRow);
    inspector.removeFromTop(3); std::array<Numeric*,12> controls { &padStart,&padEnd,&padGain,&padPan,&padSemi,&padCents,&padTime,&padOutput,&padCutoff,&padResonance,&padAttack,&padRelease };
    const int rowHeight=inspector.getHeight()/3;
    for(int row=0;row<3;++row) { auto r=inspector.removeFromTop(row==2?inspector.getHeight():rowHeight); const int colWidth=r.getWidth()/4; for(int col=0;col<4;++col) { auto cell=r.removeFromLeft(col==3?r.getWidth():colWidth); controls[(size_t)(row*4+col)]->setBounds(cell.reduced(2,0)); } }
    auto v=view; v.width=editor.getWidth(); v.height=editor.getHeight(); if(v.width!=shot.session.view.width || v.height!=shot.session.view.height) setView(v);
}

void SliceLabAudioProcessorEditor::UI::paint(juce::Graphics& g) {
    g.fillAll(ink);
    auto panelBox=[&](juce::Rectangle<int> r) { g.setColour(panel); g.fillRoundedRectangle(r.toFloat(),6); g.setColour(line.withAlpha(0.55f)); g.drawRoundedRectangle(r.toFloat().reduced(0.5f),6,1); };
    panelBox(globalBounds); panelBox(waveBounds); panelBox(stemBounds); panelBox(padBounds); panelBox(inspectorBounds);
    g.setColour(copper); g.fillRoundedRectangle((float)headerBounds.getX()+1,(float)headerBounds.getY()+8,4,22,2);
    g.setFont(font(20,true)); g.setColour(lightText); g.drawText("SLICE",headerBounds.getX()+13,headerBounds.getY()+3,60,27,juce::Justification::left);
    g.setColour(copper); g.drawText("LAB",headerBounds.getX()+73,headerBounds.getY()+3,48,27,juce::Justification::left);
    const auto fileName=shot.session.sourcePath.isNotEmpty()?juce::File(shot.session.sourcePath).getFileName():juce::String("No source loaded");
    g.setFont(font(11,true)); g.setColour(lightText); g.drawFittedText(fileName,headerBounds.getX()+132,headerBounds.getY()+2,std::max(60,headerBounds.getWidth()-435),17,juce::Justification::left,1);
    juce::String detail;
    if (shot.asset) {
        const auto confidence=shot.asset->keyConfidenceLabel.isNotEmpty() ? shot.asset->keyConfidenceLabel : juce::String((int)(shot.asset->keyConfidence*100))+"% native";
        detail=juce::String(shot.asset->durationSeconds(),2)+" s  |  "+shot.asset->detectedKey+"  "+confidence;
        if(shot.asset->detectedBpm>0) detail += "  |  detected "+juce::String(shot.asset->detectedBpm,1)+" BPM";
    } else if(shot.session.sourcePath.isNotEmpty()) detail="Source missing. Use FILE > Relink";
    else detail="Open or drop an audio file";
    g.setFont(font(10)); g.setColour(muted); g.drawFittedText(detail,headerBounds.getX()+132,headerBounds.getY()+20,std::max(60,headerBounds.getWidth()-435),15,juce::Justification::left,1);
    g.setFont(font(9,true)); g.setColour(muted); g.drawText("PADS  /  MIDI 36-67",padBounds.getX()+9,padBounds.getY()+3,200,14,juce::Justification::left);
    const auto selectedCount=std::popcount(view.selectionMask);
    g.drawText("PAD "+juce::String(shot.selectedPad+1)+(selectedCount>1?"  /  "+juce::String(selectedCount)+" SELECTED":"")+"  /  INSPECTOR",inspectorBounds.getX()+9,inspectorBounds.getY()+3,inspectorBounds.getWidth()-18,14,juce::Justification::left);
    const auto& glob=shot.session.global;
    juce::String status=shot.error.isNotEmpty()?shot.error:shot.status;
    if(shot.busy && shot.progress>=0) status += "  "+juce::String((int)std::round(shot.progress*100))+"%";
    if(shot.asset && !shot.asset->stemsReady) status += "  |  stems not ready";
    if(shot.asset && shot.asset->detectedBpm<=0) status += "  |  BPM not detected, set source BPM";
    const bool hostAvailable=shot.hostBpm>1.0;
    const double bpm=glob.sync && hostAvailable?shot.hostBpm*glob.hostMultiplier:glob.targetBpm;
    status += "  |  output "+juce::String(bpm,1)+" BPM";
    if (glob.sync && !hostAvailable) status += "  |  host tempo unavailable";
    const std::array<juce::String,12> names { "C","C#","D","D#","E","F","F#","G","G#","A","A#","B" };
    int keyIndex=sourceKeyClass(glob,shot.asset.get()); const bool minor=keyIndex>=12;
    if(keyIndex>=0) {
        const auto& p=shot.session.pads[(size_t)juce::jlimit(0,padCount-1,shot.selectedPad)];
        const double tune=glob.semitones+p.semitones+(glob.cents+p.cents)/100.0;
        const double pitch=juce::jlimit(0.25,4.0,std::pow(2.0,tune/12.0));
        double localBpm=glob.sourceBpm;
        const auto& anchors=shot.session.beatAnchors;
        if (anchors.size()>=2) {
            auto upper=std::upper_bound(anchors.begin(),anchors.end(),p.startSeconds-glob.beatOffsetSeconds,[](double t,const BeatAnchor& a){return t<a.sourceSeconds;});
            if(upper==anchors.begin()) ++upper;
            if(upper==anchors.end()) --upper;
            const auto& a=*(upper-1); const auto& b=*upper;
            localBpm=60.0*(b.beat-a.beat)/(b.sourceSeconds-a.sourceSeconds);
        }
        const double ratio=juce::jlimit(0.25,4.0,(double)p.timeRatio*localBpm/std::max(1.0,bpm));
        const double shift=12.0*std::log2(glob.repitch ? pitch/ratio : pitch);
        const int semitone=(int)std::round(shift); const int outputKey=((keyIndex%12+semitone)%12+12)%12;
        juce::String audibleKey="Key "+names[(size_t)outputKey]+(minor?" minor":" major");
        const int cents=(int)std::round((shift-semitone)*100); if(cents!=0) audibleKey += " "+juce::String(cents>0?"+":"")+juce::String(cents)+"c";
        status=audibleKey+"  |  "+status;
    }
    g.setFont(font(10)); g.setColour(shot.error.isNotEmpty()?juce::Colour(0xffffa292):muted);
    auto statusArea=statusBounds; g.drawFittedText(status,statusArea.removeFromLeft(std::max(100,statusArea.getWidth()-155)),juce::Justification::centredLeft,1);
    auto meter=statusArea.reduced(4,7).toFloat(); g.setColour(raised); g.fillRoundedRectangle(meter,2); g.setColour(copper); g.fillRoundedRectangle(meter.withWidth(meter.getWidth()*juce::jlimit(0.0f,1.0f,shot.outputPeak)),2);
}
void SliceLabAudioProcessorEditor::UI::openFileMenu() {
    juce::PopupMenu menu; menu.addItem(1,"Open audio..."); menu.addItem(2,"Relink missing source...",!shot.asset && shot.session.sourceHash.isNotEmpty()); menu.addSeparator();
    menu.addItem(3,"Save preset..."); menu.addItem(4,"Load preset..."); menu.addSeparator(); menu.addItem(8,"Import markers...",shot.asset!=nullptr); menu.addItem(5,"Export selected pad as WAV...",shot.asset!=nullptr); menu.addItem(6,"Export pads as MIDI...",shot.asset!=nullptr); menu.addItem(7,"Collect source and stems...",shot.asset!=nullptr);
    auto safe=juce::Component::SafePointer<SliceLabAudioProcessorEditor>(&editor);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&file),[safe](int result){ if(safe==nullptr || !safe->ui) return; switch(result) {case 1:safe->ui->chooseFile(0);break;case 2:safe->ui->chooseFile(6);break;case 3:safe->ui->chooseFile(1);break;case 4:safe->ui->chooseFile(2);break;case 5:safe->ui->chooseFile(3);break;case 6:safe->ui->chooseFile(4);break;case 7:safe->ui->chooseFile(5);break;case 8:safe->ui->chooseFile(7);break;default:break;} });
}
void SliceLabAudioProcessorEditor::UI::chooseFile(int action) {
    juce::String title, filter; int flags=0; juce::File initial;
    switch(action) {
        case 0: case 6: title=action==0?"Open source audio":"Relink source audio"; filter="*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg"; flags=juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles; break;
        case 1: title="Save SliceLab preset"; filter="*.slicelab"; initial=juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("SliceLab.slicelab"); flags=juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::warnAboutOverwriting; break;
        case 2: title="Load SliceLab preset"; filter="*.slicelab"; flags=juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles; break;
        case 3: title="Export selected pad"; filter="*.wav"; initial=juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Slice.wav"); flags=juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::warnAboutOverwriting; break;
        case 4: title="Export MIDI"; filter="*.mid"; initial=juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("SliceLab.mid"); flags=juce::FileBrowserComponent::saveMode|juce::FileBrowserComponent::canSelectFiles|juce::FileBrowserComponent::warnAboutOverwriting; break;
        case 5: title="Collect assets into a folder"; flags=juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectDirectories; break;
        case 7: title="Import markers"; filter="*.csv;*.tsv;*.txt"; flags=juce::FileBrowserComponent::openMode|juce::FileBrowserComponent::canSelectFiles; break;
        default: return;
    }
    chooser=std::make_unique<juce::FileChooser>(title,initial,filter);
    auto safe=juce::Component::SafePointer<SliceLabAudioProcessorEditor>(&editor); const int selected=shot.selectedPad;
    chooser->launchAsync(flags,[safe,action,selected](const juce::FileChooser& fc) {
        if(safe==nullptr) return;
        auto result=fc.getResult();
        if(result==juce::File()) return;
        if(action==1 && result.getFileExtension().isEmpty()) result=result.withFileExtension(".slicelab");
        if(action==3 && result.getFileExtension().isEmpty()) result=result.withFileExtension(".wav");
        if(action==4 && result.getFileExtension().isEmpty()) result=result.withFileExtension(".mid");
        switch(action) {case 0:safe->processor.loadSample(result);break;case 6:safe->processor.relinkSample(result);break;case 1:safe->processor.savePreset(result);break;case 2:safe->processor.loadPreset(result);break;case 3:safe->processor.exportPad(selected,result);break;case 4:safe->processor.exportMidi(result);break;case 5:safe->processor.collectSample(result);break;case 7: {
            const auto status = safe->processor.importMarkers(result);
            if (status.failed()) juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Marker import", status.getErrorMessage(), "OK", safe.getComponent());
            if (safe->ui) safe->ui->refresh();
            break;
        } default:break;}
    });
}
void SliceLabAudioProcessorEditor::UI::editBeatGrid() {
    juce::DialogWindow::LaunchOptions options; options.content.setOwned(new BeatEditor(editor)); options.dialogTitle="SliceLab beat grid"; options.dialogBackgroundColour=ink; options.escapeKeyTriggersCloseButton=true; options.useNativeTitleBar=true; options.resizable=false; options.componentToCentreAround=&editor; options.launchAsync();
}
bool SliceLabAudioProcessorEditor::UI::textEditing() const {
    auto* focus=juce::Component::getCurrentlyFocusedComponent();
    return dynamic_cast<juce::TextEditor*>(focus)!=nullptr || (focus!=nullptr && focus->findParentComponentOfClass<juce::TextEditor>()!=nullptr);
}
bool SliceLabAudioProcessorEditor::UI::keyPressed(const juce::KeyPress& keyPress) {
    if(textEditing()) return false;
    const auto mods=keyPress.getModifiers();
    if(mods.isCommandDown() || mods.isCtrlDown()) {
        if(keyPress.getKeyCode()=='z' || keyPress.getKeyCode()=='Z') { if(mods.isShiftDown()) processor.redo(); else processor.undo(); refresh(); return true; }
        if(keyPress.getKeyCode()=='y' || keyPress.getKeyCode()=='Y') { processor.redo(); refresh(); return true; }
        if(keyPress.getKeyCode()=='o' || keyPress.getKeyCode()=='O') { chooseFile(0); return true; }
        return false;
    }
    if(keyPress==juce::KeyPress::escapeKey) {processor.stopAll();processor.stopSourcePreview();return true;}
    if(keyPress==juce::KeyPress::spaceKey) {if(shot.asset) {if(shot.previewPlaying) {cursor=shot.playheadSeconds; processor.stopSourcePreview();} else processor.auditionSource(cursor);} return true;}
    if(keyPress==juce::KeyPress::deleteKey || keyPress==juce::KeyPress::backspaceKey) {processor.deletePad(shot.selectedPad);refresh();return true;}
    if(mods.isAltDown()) return false;
    const auto c=juce::CharacterFunctions::toLowerCase(keyPress.getTextCharacter());
    for(int i=0;i<padCount;++i) if(c==(juce::juce_wchar)keyboardKeys[i]) {
        if(!keyDown[(size_t)i]) {
            const auto settings=processor.snapshot().session.global;
            const int note=settings.keyboardMode ? settings.rootMidiNote+i : -1;
            if (note>127) return true;
            keyDown[(size_t)i]=true; keyNotes[(size_t)i]=note;
            if(note>=0) processor.triggerNote(note,1.0f,true); else processor.triggerPad(i,1.0f,true);
        }
        return true;
    }
    return false;
}
void SliceLabAudioProcessorEditor::UI::releaseKeys() {
    for (int i=0;i<padCount;++i) if (keyDown[(size_t)i]) {
        keyDown[(size_t)i]=false; const int note=keyNotes[(size_t)i];
        if(note>=0) processor.triggerNote(note,0,false); else processor.triggerPad(i,0,false);
    }
}
bool SliceLabAudioProcessorEditor::UI::keyStateChanged() {
    bool changed=false;
    for(int i=0;i<padCount;++i) if(keyDown[(size_t)i] && !juce::KeyPress::isKeyCurrentlyDown((int)keyboardKeys[i])) {
        keyDown[(size_t)i]=false; const int note=keyNotes[(size_t)i];
        if(note>=0) processor.triggerNote(note,0,false); else processor.triggerPad(i,0,false);
        changed=true;
    }
    return changed;
}

SliceLabAudioProcessorEditor::SliceLabAudioProcessorEditor(SliceLabAudioProcessor& p) : AudioProcessorEditor(p), processor(p) {
    const auto saved=p.snapshot().session.view;
    setWantsKeyboardFocus(true); setResizable(true,true); setResizeLimits(960,680,2200,1500);
    setSize(juce::jlimit(960,2200,saved.width),juce::jlimit(680,1500,saved.height));
    ui=std::make_unique<UI>(*this); resized(); startTimerHz(30);
}
SliceLabAudioProcessorEditor::~SliceLabAudioProcessorEditor() { stopTimer(); ui.reset(); }
void SliceLabAudioProcessorEditor::paint(juce::Graphics& g) { if(ui) ui->paint(g); else g.fillAll(ink); }
void SliceLabAudioProcessorEditor::resized() { if(ui) ui->resized(); }
bool SliceLabAudioProcessorEditor::keyPressed(const juce::KeyPress& key) { return ui && ui->keyPressed(key); }
bool SliceLabAudioProcessorEditor::keyStateChanged(bool) { return ui && ui->keyStateChanged(); }
void SliceLabAudioProcessorEditor::focusLost(juce::Component::FocusChangeType) { if(ui) ui->releaseKeys(); }
bool SliceLabAudioProcessorEditor::isInterestedInFileDrag(const juce::StringArray& files) { return !files.isEmpty(); }
void SliceLabAudioProcessorEditor::filesDropped(const juce::StringArray& files,int,int) { if(!files.isEmpty()) processor.loadSample(juce::File(files[0])); }
void SliceLabAudioProcessorEditor::timerCallback() { if(ui) ui->refresh(); }
