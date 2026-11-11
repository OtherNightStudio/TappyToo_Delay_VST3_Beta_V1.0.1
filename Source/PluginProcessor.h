#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "TapeEngine.h"

class TappyProcessor final : public juce::AudioProcessor, private juce::AudioProcessorValueTreeState::Listener {
  public:
    TappyProcessor();
    ~TappyProcessor() override;
    void prepareToPlay(double, int) override;
    void releaseResources() override { hostPlaying = false; }
    void reset() override;
    void processBlock(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
    void processBlockBypassed(juce::AudioBuffer<float> &, juce::MidiBuffer &) override;
    bool isBusesLayoutSupported(const BusesLayout &) const override;
    juce::AudioProcessorEditor *createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Tappy Too Stereo 4Tap Delay"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return "Panel"; }
    void changeProgramName(int, const juce::String &) override {}
    void getStateInformation(juce::MemoryBlock &) override;
    void setStateInformation(const void *, int) override;
    juce::Result savePreset(const juce::File &);
    juce::Result loadPreset(const juce::File &);
    juce::String presetName() const;
    unsigned presetRevision() const { return nameRevision.load(); }
    juce::AudioProcessorParameter *getBypassParameter() const override {
        return state.getParameter("bypass");
    }
    static juce::String laneId(int h) { return "head" + juce::String(h + 1); }
    static juce::String enableId(int h) { return "tap" + juce::String(h + 1); }
    void snapPanelTiming();
    float panelValue(const juce::String &id) const;
    static juce::String frequencyText(float hz);
    bool isTransportPlaying() const { return hostPlaying.load(); }
    juce::AudioProcessorValueTreeState state;
    struct DisplayClock {
        double beat, turns, bpm;
    };
    DisplayClock displayClock() const;

  private:
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    void render(juce::AudioBuffer<float> &, bool);
    tappy::Controls controls() const;
    juce::ValueTree snapshot();
    void restore(juce::ValueTree);
    void setPresetName(const juce::String &);
    mutable juce::CriticalSection nameLock;
    juce::String reelName;
    std::atomic<unsigned> nameRevision{0};
    void refreshTiming();
    void parameterChanged(const juce::String &, float) override;
    using Raw = std::atomic<float> *;
    std::array<Raw, tappy::heads> head{}, enable{};
    Raw speed{}, input{}, feedback{}, feedbackLP{}, oversampling{}, mix{}, snap{}, bypass{};
    Raw tapePressure{}, crossFeedback{};
    std::array<std::atomic<float>, tappy::heads> headSetting{};
    std::atomic<float> speedSetting{1};
    tappy::TapeEngine tape;
    double fs = 48000, beat = 0, visibleTurns = 0;
    std::atomic<bool> hostPlaying{false};
    std::atomic<double> clockBpm{120}, clockBeat{0}, clockTurns{0}, clockRate{0}, clockTime{0}, blockSeconds{0};
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TappyProcessor)
};
