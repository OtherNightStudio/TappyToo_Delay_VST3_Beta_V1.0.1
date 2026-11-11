#pragma once
#include "PluginProcessor.h"

class TappyEditor final : public juce::AudioProcessorEditor, private juce::Timer {
  public:
    explicit TappyEditor(TappyProcessor &);
    ~TappyEditor() override;
    void paint(juce::Graphics &) override;
    void resized() override;

  private:
    enum class Kind { lane, toggle, knob, feedback, speed, reel, os2, os4 };
    class Control;
    TappyProcessor &proc;
    juce::Component surface;
    std::unique_ptr<juce::Drawable> print;
    juce::Image printCache;
    std::unique_ptr<juce::Drawable> strip;
    juce::Image labelCache;
    unsigned labelRevision = 0;
    int labelPixels = 0;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File lastFolder;
    void browsePreset(bool save);
    void presetMenu();
    void drawReelLabel(juce::Graphics &, float scale, double turns);
    std::vector<std::unique_ptr<Control>> handles;
    juce::TooltipWindow tooltip{this, 420};
    double lastReelTurns = -1.;
    void add(const juce::String &, Kind, const juce::String &, juce::Rectangle<int>);
    float value(const juce::String &id) const { return proc.panelValue(id); }
    double speedSetting() const { return value("speed"); }
    void timerCallback() override;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TappyEditor)
};
