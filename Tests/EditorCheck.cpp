#include "../Source/PluginEditor.h"
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
struct Transport : juce::AudioPlayHead {
    double ppq = 0, loopEnd = .25;
    double bpm = 90;
    bool playing = true, looping = false;
    juce::Optional<PositionInfo> getPosition() const override {
        PositionInfo info;
        info.setBpm(bpm);
        info.setTimeSignature(TimeSignature{3, 8});
        info.setIsLooping(looping);
        if (looping) info.setLoopPoints(LoopPoints{0., loopEnd});
        info.setPpqPosition(ppq);
        info.setIsPlaying(playing);
        return info;
    }
};
juce::Component *find(juce::Component &root, const juce::String &name) {
    if (root.getName() == name)
        return &root;
    for (auto *child : root.getChildren())
        if (auto *found = find(*child, name))
            return found;
    return nullptr;
}
void click(juce::Component &root, const juce::String &name, float panelX, float panelY = -1) {
    auto *c = find(root, name);
    check(c != nullptr, "missing editor control");
    const juce::Point<float> pos{panelX - c->getX(), panelY >= 0 ? panelY - c->getY() : c->getHeight() * .5f};
    auto now = juce::Time::getCurrentTime();
    juce::MouseEvent event(juce::Desktop::getInstance().getMainMouseSource(), pos,
                           juce::ModifierKeys::leftButtonModifier, 1, 0, 0, 0, 0, c, c, now, pos, now, 1,
                           false);
    c->mouseDown(event);
    c->mouseUp(event);
}
void checkReelGesture(juce::Component &reel, TappyProcessor &p, bool playing) {
    struct Listener : juce::AudioProcessorParameter::Listener {
        std::vector<char> events;
        void parameterValueChanged(int, float) override { events.push_back('v'); }
        void parameterGestureChanged(int, bool start) override { events.push_back(start ? '+' : '-'); }
    } listener;
    auto *param = p.state.getParameter("tapePressure");
    param->addListener(&listener);
    const juce::Point<float> start{905.f - reel.getX(), 216.f - reel.getY()};
    auto now = juce::Time::getCurrentTime();
    const auto event = [&](float dy) {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), start.translated(0.f, dy),
            juce::ModifierKeys::leftButtonModifier, 1, 0, 0, 0, 0, &reel, &reel, now, start, now, 1, true);
    };
    reel.mouseDown(event(0));
    const auto click = p.panelValue("tapePressure");
    reel.mouseDrag(event(75));
    const auto heavy = p.panelValue("tapePressure");
    reel.mouseDrag(event(-32));
    const auto light = p.panelValue("tapePressure");
    reel.mouseUp(event(-32));
    param->removeListener(&listener);
    check(playing ? click == 64 && heavy == 127 && light == 32 : click == 0 && heavy == 0 && light == 0,
          "a click immediately brakes and a vertical drag changes pressure only while playing");
    check(p.panelValue("tapePressure") == 0 && p.panelValue("speed") == 1, "release restores free motion");
    check(playing ? listener.events == std::vector<char>{'+','v','v','v','v','-'} : listener.events.empty(),
          "the host records click pressure and the final zero within one gesture");
}
void checkLiveRecordModes(TappyProcessor &p) {
    p.prepareToPlay(48000, 61);
    auto *os = p.state.getParameter("oversampling");
    p.state.getParameter("mix")->setValueNotifyingHost(1.f);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> block(2, 61);
    int n = 0;
    const auto wave = [](int time) { return float(.2 * std::sin(time * .043)); };
    for (const int choice : {0, 1, 2, 0, 2, 1, 0, 2}) {
        os->setValueNotifyingHost(os->convertTo0to1(float(choice)));
        // Include requests made during the preceding transition.
        const int blocks = choice == 1 ? 1 : 12;
        for (int i = 0; i < blocks; ++i) {
            for (int ch = 0; ch < 2; ++ch)
                for (int s = 0; s < block.getNumSamples(); ++s)
                    block.setSample(ch, s, (ch ? -1.f : 1.f) * wave(n + s));
            p.processBlock(block, midi);
            for (int s = 0; s < block.getNumSamples(); ++s) {
                const int t = n + s - tappy::TapeEngine::latency;
                const float reference = t >= 0 ? wave(t) : 0.f;
                for (int ch = 0; ch < 2; ++ch)
                    check(std::abs(block.getSample(ch, s) - (ch ? -reference : reference)) < .0001f,
                          "continuous stereo monitor and constant latency while switching record modes");
            }
            n += block.getNumSamples();
        }
    }
}
} // namespace
int main(int argc, char **argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    try {
        Transport host;
        TappyProcessor p;
        p.setPlayHead(&host);
        p.prepareToPlay(48000, 64);
        juce::AudioBuffer<float> b(2, 64);
        b.clear();
        juce::MidiBuffer events;
        p.processBlock(b, events);
        std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
        editor->setVisible(true);
        auto read = [&](const char *key) { return p.state.getRawParameterValue(key)->load(); };
        check(editor->getWidth() == 1080 && editor->getHeight() == 387 && editor->isResizable(),
              "default resizable editor");
        auto *head1 = find(*editor, "Head 1");
        check(head1 != nullptr, "first lane");
        for (const auto size : {juce::Point<int>{1080, 387}, juce::Point<int>{604, 216}, juce::Point<int>{900, 400}}) {
            editor->setSize(size.x, size.y);
            auto *surface = head1->getParentComponent();
            const auto transform = surface->getTransform();
            check(transform.mat00 == transform.mat11, "uniform panel scale");
            const std::array<int, 4> rows{126, 186, 247, 307};
            for (int h = 0; h < 4; ++h) {
                auto *lane = find(*editor, "Head " + juce::String(h + 1));
                for (int x : {131, 397, 664}) {
                    auto hit = editor->getLocalPoint(surface, juce::Point<int>{x, rows[h]});
                    check(editor->getComponentAt(hit) == lane, "resized lane hit area");
                }
            }
            const auto checkHit = [&](const char *name, juce::Point<int> position) {
                auto hit = editor->getLocalPoint(surface, position);
                check(editor->getComponentAt(hit) == find(*editor, name), "printed control hit area");
            };
            checkHit("2x oversampling", {1115, 34});
            checkHit("2x oversampling", {1127, 34});
            checkHit("4x oversampling", {1115, 50});
            checkHit("4x oversampling", {1127, 50});
            checkHit("Feedback LP", {1130, 386});
            checkHit("Cross-feedback", {1170, 386});
            checkHit("Tape Pressure", {905, 136});
            checkHit("Reel speed", {1090, 216});
            auto *corner = editor->getComponentAt(size.x - 2, size.y - 2);
            check(dynamic_cast<juce::ResizableCornerComponent *>(corner) != nullptr, "resize grip");
        }
        for (int h = 0; h < tappy::heads; ++h)
            check(p.state.getRawParameterValue(TappyProcessor::enableId(h))->load() == 1, "tap enabled by default");
        editor->setSize(1207, 433);
        const auto pixel = [&](int x, int y) {
            return editor->createComponentSnapshot({x, y, 1, 1}, false).getPixelAt(0, 0);
        };
        const auto syncButton = pixel(1157, 35);
        check(p.presetName().isEmpty(), "fresh reel is unnamed");
        auto hasMaskingTape = [&] {
            const auto reel = editor->createComponentSnapshot({755, 65, 301, 301}, false);
            for (int y = 0; y < reel.getHeight(); y += 3)
                for (int x = 0; x < reel.getWidth(); x += 3)
                    if (reel.getPixelAt(x, y) == juce::Colour(0xffe8dfba))
                        return true;
            return false;
        };
        check(!hasMaskingTape(), "fresh reel is bare");
        check(p.state.getParameter("lfo1Wave") == nullptr && p.state.getParameter("feedbackDepth") == nullptr,
              "retired modulation is absent from host automation");
        auto *reel = find(*editor, "Tape Pressure");
        check(reel != nullptr && read("tapePressure") == 0 && read("crossFeedback") == 0
              && p.state.getParameter("reelPush") == nullptr, "fresh straight return and released pressure");
        checkReelGesture(*reel, p, true);
        click(*editor, "Tape Pressure", 905, 136);
        check(!juce::PopupMenu::dismissAllActiveMenus(), "playing click never opens presets");
        host.playing = false;
        p.processBlock(b, events);
        checkReelGesture(*reel, p, false);
        check(!juce::PopupMenu::dismissAllActiveMenus(), "stopped drag never opens presets");
        click(*editor, "Tape Pressure", 905, 136);
        check(juce::PopupMenu::dismissAllActiveMenus(), "stopped click opens presets");
        host.playing = true;
        p.processBlock(b, events);
        check(pixel(1157, 50) == juce::Colour(0xff47474a), "unlit sync LED");
        check(read("bypass") == 0, "effect active by default");
        check(pixel(327, 35) == juce::Colour(0xff8dbc73), "active signal is green");
        click(*editor, "Bypass", 317);
        check(read("bypass") == 1 && pixel(307, 35) == juce::Colour(0xffdf9177), "dry bypass is red");
        click(*editor, "Bypass", 317);
        check(p.displayClock().bpm == 90, "host BPM");
        check(read("oversampling") == 1 && std::abs(read("feedbackLP") - 12500) < .02f, "legacy tone defaults");
        click(*editor, "2x oversampling", 1116, 34);
        check(read("oversampling") == 0, "active 2x switches off");
        click(*editor, "2x oversampling", 1116, 34);
        click(*editor, "4x oversampling", 1116, 50);
        check(read("oversampling") == 2, "4x replaces 2x");
        check(pixel(1127, 34) == juce::Colour(0xff03090b) && pixel(1127, 50) == juce::Colour(0xffdf9177),
              "only the selected oversampling indicator is lit");
        click(*editor, "4x oversampling", 1116, 50);
        check(read("oversampling") == 0, "active 4x switches off");
        click(*editor, "4x oversampling", 1116, 50);
        auto *filter = p.state.getParameter("feedbackLP");
        check(filter->getText(filter->convertTo0to1(999), 0) == "999 Hz" &&
              filter->getText(filter->convertTo0to1(1000), 0) == "1.0 kHz", "frequency units");
        find(*editor, "Feedback LP")->keyPressed(juce::KeyPress(juce::KeyPress::leftKey));
        const float baseFeedback = read("feedback");
        find(*editor, "Cross-feedback")->keyPressed(juce::KeyPress(juce::KeyPress::rightKey));
        check(std::abs(read("crossFeedback") - 1) < .001 && read("feedback") == baseFeedback,
              "routing knob moves independently from the regeneration fader");
        const float savedCutoff = read("feedbackLP");
        check(savedCutoff < 12500, "filter knob responds");
        click(*editor, "Head 1", 253.8f);
        check(std::abs(read("head1") + 7) < .001f, "left eighth tick");
        click(*editor, "Head 1", 257.f);
        const float free = read("head1");
        check(std::abs(free - std::round(free)) > .1, "free movement");
        click(*editor, "Reel speed", 1090, 232);
        check(read("speed") > .55f && read("speed") < .61f, "continuous reel speed");
        click(*editor, "Snap to divisions", 1157);
        check(read("head1") == -7 && read("speed") == .5625f, "engaging sync commits the nearest ticks");
        click(*editor, "Snap to divisions", 1157);
        check(read("sync") == 0 && read("head1") == -7 && read("speed") == .5625f, "releasing sync holds timing");
        click(*editor, "Head 1", 257.f);
        check(read("head1") == free, "free editing resumes from the tick");
        click(*editor, "Snap to divisions", 1157);
        // Inner endpoints read directly from the 17 radial lines in the V3 artwork.
        const std::array<juce::Point<float>, 17> ticks{{
            {1065.061f,120.545f},{1071.161f,131.537f},{1076.505f,142.915f},{1081.068f,154.628f},
            {1084.831f,166.622f},{1087.776f,178.843f},{1089.888f,191.235f},{1091.16f,203.741f},
            {1091.585f,216.305f},{1091.16f,228.868f},{1089.888f,241.374f},{1087.776f,253.766f},
            {1084.831f,265.987f},{1081.068f,277.981f},{1076.505f,289.694f},{1071.161f,301.073f},
            {1065.061f,312.064f}}};
        for (size_t i = 0; i < ticks.size(); ++i) {
            click(*editor, "Reel speed", ticks[i].x, ticks[i].y);
            check(read("speed") == float(i) / 16, "every V3 speed tick selects its detent");
        }
        click(*editor, "Reel speed", 1090, 232);
        check(read("speed") == .5625f, "speed snaps on the arc");
        auto *speedControl = find(*editor, "Reel speed");
        speedControl->keyPressed(juce::KeyPress(juce::KeyPress::leftKey));
        check(read("speed") == .5f, "one sixteenth per keypress");
        click(*editor, "Head 1", 257.f);
        check(read("head1") == -7, "division lock");
        click(*editor, "Tap 3 active", 243);
        click(*editor, "Tap 4 active", 327);
        click(*editor, "Head 2", 417.9f);
        check(std::abs(read("head2") - 1.f) < .001f, "first right tick");
        click(*editor, "Head 2", 561.5f);
        check(read("head2") == 8, "right dotted eighth");
        check(read("tap1") == 1 && read("tap2") == 1 && read("tap3") == 0 && read("tap4") == 0,
              "tap switches");
        host.ppq = 34.;
        p.processBlock(b, events);
        check(p.displayClock().beat < .1, "heartbeat on host downbeat");
        const auto lit = pixel(1157, 50);
        check(lit.getRed() > lit.getGreen() + 80 && pixel(1157, 35) == syncButton, "only LED pulses red");
        host.ppq = 34.5;
        p.processBlock(b, events);
        check(p.displayClock().beat > .49 && p.displayClock().beat < .6, "heartbeat follows seek");
        check(pixel(1157, 50).getRed() < lit.getRed() && pixel(1157, 35) == syncButton, "LED pulse decays");
        juce::MemoryBlock data;
        p.getStateInformation(data);
        TappyProcessor restored;
        restored.setStateInformation(data.getData(), int(data.getSize()));
        check(restored.state.getRawParameterValue("head2")->load() == 8 &&
                  restored.state.getRawParameterValue("sync")->load() == 1 &&
                  restored.state.getRawParameterValue("tap3")->load() == 0 &&
                  restored.state.getRawParameterValue("feedbackLP")->load() == savedCutoff &&
                  restored.state.getRawParameterValue("oversampling")->load() == 2,
              "session recall");
        auto legacy = TappyProcessor::getXmlFromBinary(data.getData(), int(data.getSize()));
        legacy->setAttribute("schema", 4);
        legacy->removeChildElement(legacy->getChildByAttribute("id", "crossFeedback"), true);
        auto *oldDepth = legacy->getChildByAttribute("id", "tapePressure");
        oldDepth->setAttribute("id", "reelPush");
        oldDepth->setAttribute("value", .95);
        restored.state.getParameter("tapePressure")->setValueNotifyingHost(.85f);
        TappyProcessor::copyXmlToBinary(*legacy, data);
        restored.setStateInformation(data.getData(), int(data.getSize()));
        check(restored.panelValue("tapePressure") == 0 && restored.panelValue("crossFeedback") == 85,
              "V3 ignores an old scrub angle and recalls its 85 percent return routing");
        legacy->setAttribute("schema", 1);
        for (auto id : {"feedbackLP", "oversampling"})
            legacy->removeChildElement(legacy->getChildByAttribute("id", id), true);
        TappyProcessor::copyXmlToBinary(*legacy, data);
        restored.setStateInformation(data.getData(), int(data.getSize()));
        check(std::abs(restored.state.getRawParameterValue("feedbackLP")->load() - 12500) < .02f &&
              restored.state.getRawParameterValue("oversampling")->load() == 1 &&
              restored.panelValue("tapePressure") == 0 && restored.panelValue("crossFeedback") == 0, "early straight return");
        restored.state.getParameter("feedback")->setValueNotifyingHost(1.f);
        check(std::isinf(restored.getTailLengthSeconds()), "sustaining loop reports an infinite tail");
        TappyProcessor switching;
        checkLiveRecordModes(switching);
        auto automate = [&](const char *id, float v) {
            auto *param = switching.state.getParameter(id);
            param->setValueNotifyingHost(param->convertTo0to1(v));
        };
        automate("head4", -3.4f);
        automate("speed", .68f);
        automate("sync", 1.f);
        automate("head4", 5.3f);
        automate("sync", 0.f);
        check(switching.panelValue("head4") == 5.f && switching.panelValue("speed") == .6875f,
              "host-automated sync release holds the last played divisions");
        switching.getStateInformation(data);
        restored.setStateInformation(data.getData(), int(data.getSize()));
        check(restored.panelValue("head4") == 5.f && restored.panelValue("speed") == .6875f,
              "free timing after host sync automation survives recall");
        automate("head4", 6.24f);
        check(std::abs(switching.panelValue("head4") - 6.24f) < .001f, "later host automation moves freely");
        juce::TemporaryFile saved(juce::File::getCurrentWorkingDirectory().getChildFile("Recorded drone.vstpreset"));
        p.state.getParameter("tapePressure")->setValueNotifyingHost(.42f);
        const auto written = p.savePreset(saved.getFile());
        check(written.wasOk(), written.getErrorMessage().toRawUTF8());
        check(hasMaskingTape(), "saving adds the strip to the reel");
        check(restored.loadPreset(saved.getFile()).wasOk(), "read VST3 preset");
        check(restored.panelValue("tapePressure") == 0 && restored.panelValue("crossFeedback") == read("crossFeedback"),
              "disk presets recall a released reel and the saved routing");
        check(restored.presetName() == saved.getFile().getFileNameWithoutExtension() &&
              restored.panelValue("head2") == p.panelValue("head2") &&
              restored.panelValue("oversampling") == p.panelValue("oversampling"), "preset name and controls");
        restored.getStateInformation(data);
        switching.setStateInformation(data.getData(), int(data.getSize()));
        check(switching.presetName() == restored.presetName(), "reel name survives host session recall");
        check(saved.getFile().replaceWithText("incomplete") && restored.loadPreset(saved.getFile()).failed() &&
              restored.presetName() == switching.presetName(), "failed load preserves reel name");
        if (argc > 2) {
            const auto factory = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);
            check(p.loadPreset(factory).wasOk() && p.presetName() == "Stereo Eighths", "load shipping factory preset");
            check(read("tapePressure") == 0, "legacy presets recall a released reel");
        }
        if (argc > 3)
            check(p.savePreset(juce::File::getCurrentWorkingDirectory().getChildFile(argv[3])).wasOk(),
                  "export preset for native host compatibility check");
        if (argc > 4) {
            TappyProcessor previous;
            check(previous.loadPreset(juce::File::getCurrentWorkingDirectory().getChildFile(argv[4])).wasOk()
                  && previous.panelValue("tapePressure") == 0 && previous.panelValue("crossFeedback") == 85,
                  "V3 disk preset keeps its return routing and releases pressure");
        }
        host.playing = false;
        p.processBlock(b, events);
        const double stopped = p.displayClock().turns;
        p.processBlock(b, events);
        check(p.displayClock().turns == stopped, "reel and label park when transport stops");
        if (argc > 1) {
            editor->setSize(1207, 433);
            auto bitmap = editor->createComponentSnapshot(editor->getLocalBounds(), false);
            juce::FileOutputStream file(juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]));
            file.setPosition(0);
            file.truncate();
            check(juce::PNGImageFormat().writeImageToStream(bitmap, file), "panel render");
        }
        std::cout << "PASS: free and snapped GUI gestures, mute toggles, host BPM/seek/heartbeat, state "
                     "recall and editor render\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
