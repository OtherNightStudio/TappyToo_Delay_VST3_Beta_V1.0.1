#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <charconv>
#include <limits>

juce::String TappyProcessor::frequencyText(float hz) {
    return hz < 1000.f ? juce::String(juce::roundToInt(hz)) + " Hz"
                      : juce::String(hz / 1000.f, 1) + " kHz";
}

juce::AudioProcessorValueTreeState::ParameterLayout TappyProcessor::layout() {
    juce::AudioProcessorValueTreeState::ParameterLayout p;
    auto flag = [&](const juce::String &id, const juce::String &name, bool initial) {
        p.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{id, 1}, name, initial));
    };
    auto number = [&](const juce::String &id, const juce::String &name, float lo, float hi, float initial) {
        p.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{id, 1}, name,
                                                          juce::NormalisableRange<float>(lo, hi), initial));
    };
    number("input", "Record input (dB)", -24, 24, 0);
    number("mix", "Dry / wet", 0, 1, .35f);
    number("feedback", "Feedback", 0, 1, .35f);
    number("speed", "Reel speed", 0, 1, 1);
    flag("sync", "Sync heads to divisions", false);
    flag("bypass", "Bypass", false);
    for (int h = 0; h < tappy::heads; ++h) {
        number(laneId(h), "Head " + juce::String(h + 1) + " position", -tappy::stops, tappy::stops, 0);
        flag(enableId(h), "Tap " + juce::String(h + 1) + " active", true);
    }
    p.add(std::make_unique<juce::AudioParameterChoice>(juce::ParameterID{"oversampling", 1}, "Oversampling",
                                                       juce::StringArray{"Off", "2x", "4x"}, 1));
    juce::NormalisableRange<float> cutoff(20.f, 20000.f,
        [](float lo, float hi, float n) { return lo * std::pow(hi / lo, n); },
        [](float lo, float hi, float hz) { return std::log(hz / lo) / std::log(hi / lo); },
        [](float lo, float hi, float hz) { return std::round(std::clamp(hz, lo, hi)); });
    const auto display = juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction([](float hz, int) { return frequencyText(hz); })
        .withValueFromStringFunction([](const juce::String &text) {
            return text.getFloatValue() * (text.containsIgnoreCase("k") ? 1000.f : 1.f);
        });
    p.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"feedbackLP", 1}, "Feedback LP",
                                                      cutoff, 12500.f, display));
    p.add(std::make_unique<juce::AudioParameterInt>(juce::ParameterID{"tapePressure", 1},
                                                    "Tape Pressure", 0, tappy::maxTapePressure, 0));
    p.add(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"crossFeedback", 1}, "Cross-feedback",
        juce::NormalisableRange<float>(0.f, 100.f), 0.f, juce::AudioParameterFloatAttributes().withLabel("%")));
    return p;
}
TappyProcessor::TappyProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      state(*this, nullptr, "TappyToo", layout()) {
    for (int h = 0; h < tappy::heads; ++h) {
        head[h] = state.getRawParameterValue(laneId(h));
        enable[h] = state.getRawParameterValue(enableId(h));
    }
    speed = state.getRawParameterValue("speed");
    input = state.getRawParameterValue("input");
    feedback = state.getRawParameterValue("feedback");
    feedbackLP = state.getRawParameterValue("feedbackLP");
    oversampling = state.getRawParameterValue("oversampling");
    tapePressure = state.getRawParameterValue("tapePressure");
    crossFeedback = state.getRawParameterValue("crossFeedback");
    mix = state.getRawParameterValue("mix");
    snap = state.getRawParameterValue("sync");
    bypass = state.getRawParameterValue("bypass");
    for (int h = 0; h < tappy::heads; ++h)
        state.addParameterListener(laneId(h), this);
    state.addParameterListener("speed", this);
    state.addParameterListener("sync", this);
    refreshTiming();
    setLatencySamples(tappy::TapeEngine::latency);
}
TappyProcessor::~TappyProcessor() {
    state.removeParameterListener("sync", this);
    state.removeParameterListener("speed", this);
    for (int h = 0; h < tappy::heads; ++h)
        state.removeParameterListener(laneId(h), this);
}
void TappyProcessor::refreshTiming() {
    const bool locked = snap->load() > .5f;
    for (int h = 0; h < tappy::heads; ++h)
        headSetting[h] = float(tappy::position(head[h]->load(), locked));
    speedSetting = float(tappy::reelSpeed(speed->load(), locked));
}
void TappyProcessor::parameterChanged(const juce::String &id, float v) {
    // Host automation may arrive on the audio thread. Only update the effective positions here.
    if (id == "sync") {
        if (v > .5f)
            refreshTiming();
    } else if (id == "speed") {
        speedSetting = float(tappy::reelSpeed(v, snap->load() > .5f));
    } else {
        const int h = int(id.getLastCharacter()) - '1';
        headSetting[h] = float(tappy::position(v, snap->load() > .5f));
    }
}
float TappyProcessor::panelValue(const juce::String &id) const {
    if (id == "speed")
        return speedSetting.load();
    if (id.startsWith("head"))
        return headSetting[int(id.getLastCharacter()) - '1'].load();
    return state.getRawParameterValue(id)->load();
}
bool TappyProcessor::isBusesLayoutSupported(const BusesLayout &l) const {
    auto out = l.getMainOutputChannelSet();
    return out == l.getMainInputChannelSet() &&
           (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}
void TappyProcessor::prepareToPlay(double sampleRate, int) {
    hostPlaying = false;
    fs = sampleRate;
    tape.prepare(fs);
    beat = visibleTurns = 0;
    blockSeconds = 0;
    clockBpm = 120;
    clockBeat = clockTurns = clockTime = 0;
}
void TappyProcessor::reset() {
    hostPlaying = false;
    tape.reset();
    beat = visibleTurns = 0;
    blockSeconds = 0;
    clockBeat = clockTurns = clockTime = 0;
}
tappy::Controls TappyProcessor::controls() const {
    tappy::Controls p;
    p.tapePressure = juce::roundToInt(tapePressure->load());
    p.crossFeedback = crossFeedback->load() / 100.;
    p.inputDb = input->load();
    p.feedback = feedback->load();
    p.feedbackLP = feedbackLP->load();
    p.oversampling = 1 << int(oversampling->load());
    p.mix = mix->load();
    p.bpm = clockBpm.load();
    p.sync = snap->load() > .5f;
    p.speed = speedSetting.load();
    p.bypass = bypass->load() > .5f;
    for (int h = 0; h < tappy::heads; ++h) {
        p.lane[h] = headSetting[h].load();
        p.enabled[h] = enable[h]->load() > .5f;
    }
    return p;
}
double TappyProcessor::getTailLengthSeconds() const {
    const auto p = controls();
    double longest = 0;
    for (int h = 0; h < tappy::heads; ++h)
        if (p.enabled[h])
            longest = std::max(longest, tappy::noteLength(tappy::position(p.lane[h], p.sync)));
    const double loadedSpeed = p.loadedSpeed();
    if (loadedSpeed < .0001 || longest == 0)
        return 0;
    const double loop = tappy::feedbackGain(p.feedback);
    if (loop >= 1.)
        return std::numeric_limits<double>::infinity();
    const double repeats = loop == 0 ? 1 : 1 + std::ceil(std::log(.0001) / std::log(loop));
    const double seconds = repeats * longest * 240. / (p.bpm * loadedSpeed);
    // JUCE converts finite tails to a signed sample count in the VST3 wrapper.
    if (seconds * std::max(1., getSampleRate()) >= std::numeric_limits<int>::max())
        return std::numeric_limits<double>::infinity();
    return seconds;
}
void TappyProcessor::render(juce::AudioBuffer<float> &b, bool forceBypass) {
    juce::ScopedNoDenormals guard;
    if (b.getNumChannels() == 0 || b.getNumSamples() == 0)
        return;
    auto p = controls();
    p.bypass |= forceBypass;
    bool playing = false;
    if (auto *ph = getPlayHead())
        if (auto info = ph->getPosition()) {
            playing = info->getIsPlaying();
            if (auto bpm = info->getBpm(); bpm && std::isfinite(*bpm) && *bpm > 0)
                p.bpm = std::clamp(*bpm, 20., 400.);
            if (auto ppq = info->getPpqPosition(); playing && ppq && std::isfinite(*ppq))
                beat = *ppq - std::floor(*ppq);
        }
    hostPlaying = playing;
    const double previousTurns = tape.turns();
    tape.process(b.getWritePointer(0), b.getNumChannels() > 1 ? b.getWritePointer(1) : nullptr,
                 b.getNumSamples(), p);
    beat += b.getNumSamples() / fs * p.bpm / 60.;
    beat -= std::floor(beat);
    clockBpm = p.bpm;
    clockBeat = beat;
    if (playing) visibleTurns += tape.turns() - previousTurns;
    visibleTurns -= std::floor(visibleTurns);
    clockTurns = visibleTurns;
    clockRate = playing ? tape.transportRatio() : 0.;
    blockSeconds = b.getNumSamples() / fs;
    clockTime = juce::Time::getMillisecondCounterHiRes();
}
void TappyProcessor::processBlock(juce::AudioBuffer<float> &b, juce::MidiBuffer &) { render(b, false); }
void TappyProcessor::processBlockBypassed(juce::AudioBuffer<float> &b, juce::MidiBuffer &) {
    render(b, true);
}
TappyProcessor::DisplayClock TappyProcessor::displayClock() const {
    const double now = juce::Time::getMillisecondCounterHiRes(), stamp = clockTime.load(),
                 bpm = clockBpm.load();
    const double dt = std::max(0., (now - stamp) * .001);
    const auto wrap = [](double v) { return v - std::floor(v); };
    return {wrap(clockBeat.load() + dt * bpm / 60.), wrap(clockTurns.load() + std::min(dt, blockSeconds.load()) * clockRate.load() * 2. / 3.),
            bpm};
}
juce::ValueTree TappyProcessor::snapshot() {
    auto tree = state.copyState();
    tree.getChildWithProperty("id", "speed").setProperty("value", speedSetting.load(), nullptr);
    for (int h = 0; h < tappy::heads; ++h)
        tree.getChildWithProperty("id", laneId(h)).setProperty("value", headSetting[h].load(), nullptr);
    tree.getChildWithProperty("id", "tapePressure").setProperty("value", 0.f, nullptr);
    tree.setProperty("schema", 5, nullptr);
    tree.setProperty("presetName", presetName(), nullptr);
    return tree;
}
void TappyProcessor::getStateInformation(juce::MemoryBlock &dest) {
    if (auto xml = snapshot().createXml())
        copyXmlToBinary(*xml, dest);
}
void TappyProcessor::setStateInformation(const void *data, int size) {
    if (auto xml = getXmlFromBinary(data, size); xml && xml->hasTagName("TappyToo"))
        restore(juce::ValueTree::fromXml(*xml));
}
void TappyProcessor::restore(juce::ValueTree tree) {
    for (int i = tree.getNumChildren(); --i >= 0;) {
        const auto child = tree.getChild(i);
        if (child.hasType("PARAM") && state.getParameter(child["id"].toString()) == nullptr)
            tree.removeChild(i, nullptr);
    }
    if (auto pressure = tree.getChildWithProperty("id", "tapePressure"); pressure.isValid())
        pressure.setProperty("value", 0.f, nullptr);
    // Earlier LP-equipped builds had a fixed 85% stereo return. The oldest build
    // kept each channel on its own side. Detect the saved format before adding defaults.
    if (int(tree.getProperty("schema", 1)) < 5 && tree.getChildWithProperty("id", "feedbackLP").isValid()
        && !tree.getChildWithProperty("id", "crossFeedback").isValid())
        tree.appendChild(juce::ValueTree("PARAM", {{"id", "crossFeedback"}, {"value", 85.f}}), nullptr);
    for (auto *param : getParameters()) {
        auto *p = dynamic_cast<juce::RangedAudioParameter *>(param);
        if (p && !tree.getChildWithProperty("id", p->paramID).isValid()) {
            tree.appendChild(juce::ValueTree("PARAM", {{"id", p->paramID},
                {"value", p->convertFrom0to1(p->getDefaultValue())}}), nullptr);
        }
    }
    state.replaceState(tree);
    refreshTiming();
    auto name = tree.getProperty("presetName").toString();
    if (int(tree.getProperty("schema", 1)) < 2 && name == "Untitled") name.clear();
    setPresetName(name);
}
juce::String TappyProcessor::presetName() const {
    const juce::ScopedLock lock(nameLock);
    return reelName;
}
void TappyProcessor::setPresetName(const juce::String &name) {
    const juce::ScopedLock lock(nameLock);
    reelName = name;
    ++nameRevision;
}

namespace {
// VST3 component CID published by Tappy Too and stored in its factory presets.
constexpr char componentCID[] = "ABCDEF019182FAEB4F744E7354743444";
constexpr int headerSize = 48, entrySize = 20;
}

juce::Result TappyProcessor::savePreset(const juce::File &file) {
    const auto name = file.getFileNameWithoutExtension();
    auto tree = snapshot();
    tree.setProperty("presetName", name, nullptr);
    juce::MemoryBlock component;
    copyXmlToBinary(*tree.createXml(), component);

    juce::MemoryOutputStream preset;
    preset.write("VST3", 4);
    preset.writeInt(1);
    preset.write(componentCID, 32);
    preset.writeInt64(headerSize + static_cast<juce::int64>(component.getSize()));
    preset.write(component.getData(), component.getSize());
    preset.write("List", 4);
    preset.writeInt(1);
    preset.write("Comp", 4);
    preset.writeInt64(headerSize);
    preset.writeInt64(static_cast<juce::int64>(component.getSize()));

    juce::TemporaryFile temp(file);
    {
        juce::FileOutputStream stream(temp.getFile());
        if (!stream.openedOk() || !stream.write(preset.getData(), preset.getDataSize()))
            return juce::Result::fail("Could not write the preset file.");
        stream.flush();
        if (stream.getStatus().failed())
            return stream.getStatus();
    }
    if (!temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail("Could not replace the destination file.");
    setPresetName(name);
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    return juce::Result::ok();
}

juce::Result TappyProcessor::loadPreset(const juce::File &file) {
    juce::FileInputStream in(file);
    if (!in.openedOk())
        return in.getStatus();
    const auto invalid = juce::Result::fail("This file does not contain a valid Tappy Too preset.");
    char tag[4], cid[32];
    if (in.read(tag, 4) != 4 || std::memcmp(tag, "VST3", 4) || in.readInt() != 1 ||
        in.read(cid, 32) != 32 || std::memcmp(cid, componentCID, 32))
        return invalid;
    const auto list = in.readInt64();
    if (list < headerSize || list > in.getTotalLength() - 8 || !in.setPosition(list) ||
        in.read(tag, 4) != 4 || std::memcmp(tag, "List", 4))
        return invalid;
    const int entries = in.readInt();
    if (entries < 1 || entries > (in.getTotalLength() - in.getPosition()) / entrySize)
        return invalid;
    for (int i = 0; i < entries; ++i) {
        if (in.read(tag, 4) != 4) return invalid;
        const auto offset = in.readInt64(), bytes = in.readInt64();
        if (std::memcmp(tag, "Comp", 4) != 0) continue;
        if (offset < headerSize || offset > list || bytes < 1 || bytes > list - offset ||
            bytes > 4 * 1024 * 1024 || !in.setPosition(offset))
            return invalid;
        juce::MemoryBlock block;
        if (in.readIntoMemoryBlock(block, size_t(bytes)) != size_t(bytes)) return invalid;
        auto xml = getXmlFromBinary(block.getData(), int(bytes));
        if (!xml || !xml->hasTagName("TappyToo")) return invalid;
        auto tree = juce::ValueTree::fromXml(*xml);
        const int schema = int(tree.getProperty("schema", 1));
        juce::ValueTree controlsToLoad("TappyToo");
        for (auto *parameter : getParameters()) {
            auto *p = dynamic_cast<juce::RangedAudioParameter *>(parameter);
            if (p == nullptr) continue;
            auto child = tree.getChildWithProperty("id", p->paramID);
            if (!child.isValid() && schema < 5 && (p->paramID == "tapePressure" || p->paramID == "crossFeedback"))
                continue;
            if (!child.isValid() && schema < 2 &&
                (p->paramID == "oversampling" || p->paramID == "feedbackLP"))
                continue;
            if (!child.hasType("PARAM") || !child.hasProperty("value")) return invalid;
            const auto text = child["value"].toString().toStdString();
            double value = 0.;
            const auto end = text.data() + text.size();
            const auto parsed = std::from_chars(text.data(), end, value);
            const auto &range = p->getNormalisableRange();
            if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(value)
                || value < range.start || value > range.end) return invalid;
            controlsToLoad.appendChild(juce::ValueTree("PARAM", {{"id", p->paramID}, {"value", value}}), nullptr);
        }
        controlsToLoad.setProperty("presetName", file.getFileNameWithoutExtension(), nullptr);
        controlsToLoad.setProperty("schema", schema, nullptr);
        restore(controlsToLoad);
        updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
        return juce::Result::ok();
    }
    return invalid;
}
void TappyProcessor::snapPanelTiming() {
    const auto commit = [&](const juce::String &id, float snapped) {
        auto *param = state.getParameter(id);
        const float next = param->convertTo0to1(snapped);
        if (param->getValue() != next) {
            param->beginChangeGesture();
            param->setValueNotifyingHost(next);
            param->endChangeGesture();
        }
    };
    for (int h = 0; h < tappy::heads; ++h)
        commit(laneId(h), float(tappy::position(head[h]->load(), true)));
    commit("speed", float(tappy::reelSpeed(speed->load(), true)));
}
juce::AudioProcessorEditor *TappyProcessor::createEditor() { return new TappyEditor(*this); }
juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() { return new TappyProcessor; }
