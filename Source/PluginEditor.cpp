#include "PluginEditor.h"
#include "BinaryData.h"

namespace {
constexpr float W = 1207.3f, H = 432.6f, cx = 905.5f, cy = 216.3f;
constexpr float pi = juce::MathConstants<float>::pi;
const juce::Colour salmon(0xffdf9177), olive(0xff8dbc73), cream(0xffd2d0c6), dark(0xff29292b);
const juce::Colour panelColour(0xff353537), ledOff(0xff47474a), ledRed(0xffff5047);
const juce::Rectangle<float> panelBounds{0, 0, W, H};
const juce::Rectangle<float> reelAnimationBounds{cx - 142, cy - 142, 284, 284};
const juce::Rectangle<float> syncLedBounds{1153, 46, 8, 9};
constexpr std::array<float, tappy::heads> laneY{125.8f, 186.1f, 246.5f, 306.9f};
constexpr float speedArcHalfAngle = .54042f; // V3 arc endpoints, divided by its 16 equal speed intervals.
// Engraved tick centres, from the left whole note to the right whole note.
constexpr std::array<float, 2 * tappy::stops + 1> tickX{
    130.8f, 151.3f, 171.8f, 192.3f, 212.8f, 233.3f, 253.8f, 274.3f, 294.9f,
    315.4f, 335.9f, 356.4f, 376.9f, 397.4f, 417.9f, 438.4f, 458.9f, 479.4f,
    499.9f, 520.4f, 541.0f, 561.5f, 582.0f, 602.5f, 623.0f, 643.5f, 664.0f};
float headX(double lane) {
    const auto index = std::clamp(lane + tappy::stops, 0., double(tickX.size() - 1));
    const auto i = std::min(size_t(index), tickX.size() - 2);
    return juce::jmap(float(index - i), tickX[i], tickX[i + 1]);
}
float laneAt(float x) {
    x = std::clamp(x, tickX.front(), tickX.back());
    const auto upper = std::lower_bound(tickX.begin() + 1, tickX.end(), x);
    const auto lower = upper - 1;
    return float(lower - tickX.begin() - tappy::stops) + (x - *lower) / (*upper - *lower);
}
void disc(juce::Graphics &g, float x, float y, float r, juce::Colour colour) {
    g.setColour(colour);
    g.fillEllipse(x - r, y - r, 2 * r, 2 * r);
}
void triangle(juce::Graphics &g, juce::Point<float> a, juce::Point<float> b, juce::Point<float> c,
              juce::Colour colour) {
    juce::Path p;
    p.addTriangle(a, b, c);
    g.setColour(colour);
    g.fillPath(p);
}
void toggle(juce::Graphics &g, float x, float y, bool active) {
    g.setColour(active ? olive : salmon);
    g.fillRoundedRectangle(x - 20.1f, y - 10.1f, 40.2f, 20.2f, 10.f);
    const float thumb = x + (active ? -10.06f : 10.06f);
    disc(g, thumb, y, 10.6f, dark);
    disc(g, thumb, y, 9.7f, cream);
}
} // namespace

class TappyEditor::Control final : public juce::Component, public juce::SettableTooltipClient {
  public:
    Control(TappyEditor &e, juce::String key, Kind kind, juce::String name)
        : owner(e), id(std::move(key)), type(kind), primary(*e.proc.state.getParameter(id)) {
        setName(name);
        setTitle(name);
        setWantsKeyboardFocus(true);
        setMouseCursor(isSwitch() ? juce::MouseCursor::PointingHandCursor
                       : type == Kind::lane ? juce::MouseCursor::LeftRightResizeCursor
                                            : juce::MouseCursor::UpDownResizeCursor);
    }
    ~Control() override { end(); }
    void focusLost(FocusChangeType) override {
        if (type == Kind::reel) cancelReel();
    }
    void cancelReel() {
        reelPressed = false;
        end();
    }
    bool onReel(juce::Point<float> local) const {
        const float x = local.x + getX() - cx, y = local.y + getY() - cy;
        const float radius2 = x*x + y*y;
        // Reserve the lever root as well as its outer scale for speed gestures.
        return radius2 < 151.f * 151.f && !(radius2 >= 145.f * 145.f && std::abs(std::atan2(y, x)) < .60f);
    }
    bool hitTest(int x, int y) override {
        if (type == Kind::reel) return onReel({float(x), float(y)});
        if (type != Kind::speed) return true;
        const float dx = x + getX() - cx, dy = y + getY() - cy;
        const float distance = dx * dx + dy * dy;
        return distance >= 145.f * 145.f && distance <= 212.f * 212.f && std::abs(std::atan2(dy, dx)) <= .62f;
    }
    bool valueChangedSinceLastFrame() {
        if (type == Kind::reel && reelPressed && pressedPlaying != owner.proc.isTransportPlaying())
            cancelReel();
        const float next = currentValue();
        const bool changed = next != lastValue;
        lastValue = next;
        return changed;
    }
    juce::String getTooltip() override {
        const float v = owner.value(id);
        if (type == Kind::lane) {
            const double location = tappy::position(v, owner.value("sync") > .5f);
            const double multiplier = owner.speedSetting(), bpm = owner.proc.displayClock().bpm;
            const auto length = tappy::noteLength(location);
            juce::String text = getName() + (location < 0 ? "  L  " : location > 0 ? "  R  " : "  C  ");
            if (std::abs(location - std::round(location)) < .001)
                text += juce::String(tappy::labels[size_t(std::lround(std::abs(location)))]) + "  ";
            return text + (multiplier > 0 ? juce::String(240000. * length / (bpm * multiplier), 1) + " ms"
                                          : "stopped");
        }
        if (id == "bypass")
            return v > .5f ? "Bypassed (dry signal)" : "Delay active";
        if (type == Kind::os2 || type == Kind::os4)
            return getName() + (int(v) == choice() ? " (click to switch off)" : " (click to select)");
        if (type == Kind::reel)
            return owner.proc.isTransportPlaying() ? "Hold the reel to brake. Drag down for more pressure, up for less."
                                                   : "Click to load or save a preset.";
        if (id == "crossFeedback")
            return "Cross-feedback " + juce::String(v, 1) + "%";
        if (id == "feedbackLP")
            return "Feedback low-pass at reference speed: " + TappyProcessor::frequencyText(v);
        if (type == Kind::toggle)
            return getName() + ": " + (v > .5f ? "on" : "off");
        if (id == "input")
            return "Record input: " + juce::String(v, 1) + " dB";
        if (id == "speed")
            return "Speed " + juce::String(owner.speedSetting(), 3)
                 + "x. Drag the lever to change speed.";
        return getName() + ": " + juce::String(100 * v, 1) + "%";
    }
    void mouseDown(const juce::MouseEvent &e) override {
        grabKeyboardFocus();
        if (type == Kind::reel) {
            reelPressed = e.mods.isLeftButtonDown();
            pressedPlaying = owner.proc.isTransportPlaying();
            reelDragged = false;
            if (reelPressed && pressedPlaying) {
                begin();
                put(primary.convertTo0to1(clickPressure));
            }
            return;
        }
        begin();
        initial = currentValue();
        if (isSwitch())
            press();
        else if (type == Kind::lane || type == Kind::feedback)
            absolute(e.position);
        else if (type == Kind::speed && !e.mods.isShiftDown()) {
            arcDrag = true;
            absolute(e.position);
        }
    }
    void mouseDrag(const juce::MouseEvent &e) override {
        if (isSwitch())
            return;
        if (type == Kind::reel) {
            if (!reelPressed || (!reelDragged && e.getDistanceFromDragStart() < 3)) return;
            reelDragged = true;
            if (!pressedPlaying || !owner.proc.isTransportPlaying()) {
                cancelReel();
                return;
            }
            const float pressure = std::clamp(clickPressure + e.getDistanceFromDragStartY(),
                                              0.f, float(tappy::maxTapePressure));
            put(primary.convertTo0to1(pressure));
            return;
        }
        if (type == Kind::lane || type == Kind::feedback || arcDrag)
            absolute(e.position);
        else
            put(initial - e.getDistanceFromDragStartY() / (e.mods.isShiftDown() ? 1200.f : 200.f));
    }
    void mouseUp(const juce::MouseEvent &e) override {
        end();
        arcDrag = false;
        if (type == Kind::reel && std::exchange(reelPressed, false) && !reelDragged
            && !pressedPlaying && !owner.proc.isTransportPlaying() && onReel(e.position))
            owner.presetMenu();
    }
    void mouseDoubleClick(const juce::MouseEvent &) override {
        if (type == Kind::reel) return;
        if (!isSwitch()) {
            begin();
            put(primary.getDefaultValue());
            end();
        }
    }
    bool keyPressed(const juce::KeyPress &k) override {
        if (type == Kind::reel && k.getKeyCode() == juce::KeyPress::returnKey) {
            owner.presetMenu();
            return true;
        }
        if (type == Kind::reel) return false;
        if (isSwitch() && (k == juce::KeyPress::spaceKey || k == juce::KeyPress::returnKey)) {
            begin();
            press();
            end();
            return true;
        }
        if (k == juce::KeyPress::leftKey || k == juce::KeyPress::downKey) {
            step(-1, k.getModifiers().isShiftDown());
            return true;
        }
        if (k == juce::KeyPress::rightKey || k == juce::KeyPress::upKey) {
            step(1, k.getModifiers().isShiftDown());
            return true;
        }
        return false;
    }
    void mouseWheelMove(const juce::MouseEvent &e, const juce::MouseWheelDetails &wheel) override {
        if (type != Kind::reel && wheel.deltaY != 0)
            step(wheel.deltaY > 0 ? 1 : -1, e.mods.isShiftDown());
    }

  private:
    TappyEditor &owner;
    juce::String id;
    Kind type;
    juce::RangedAudioParameter &primary;
    static constexpr float clickPressure = 64.f;
    bool reelPressed = false, pressedPlaying = false, reelDragged = false;
    float initial = 0, lastValue = -1;
    bool gesture = false, arcDrag = false;
    float currentValue() const {
        return type == Kind::lane || type == Kind::speed ? primary.convertTo0to1(owner.value(id)) : primary.getValue();
    }
    bool isSwitch() const { return type == Kind::toggle || type == Kind::os2 || type == Kind::os4; }
    int choice() const { return type == Kind::os2 ? 1 : 2; }
    void press() {
        if (type == Kind::toggle)
            put(1.f - primary.getValue());
        else {
            const float target = primary.convertTo0to1(float(choice()));
            put(primary.getValue() == target ? 0.f : target);
        }
    }
    void begin() {
        if (!gesture) {
            primary.beginChangeGesture();
            gesture = true;
            if (type == Kind::lane || type == Kind::speed) {
                const float actual = currentValue();
                if (primary.getValue() != actual)
                    primary.setValueNotifyingHost(actual);
            }
        }
    }
    void end() {
        if (gesture) {
            if (type == Kind::reel) primary.setValueNotifyingHost(primary.convertTo0to1(0.f));
            primary.endChangeGesture();
            gesture = false;
        }
    }
    void put(float n) {
        if (id == "sync" && (n > .5f) != (primary.getValue() > .5f))
            owner.proc.snapPanelTiming();
        if (type == Kind::speed)
            n = float(tappy::reelSpeed(n, owner.value("sync") > .5f));
        primary.setValueNotifyingHost(std::clamp(n, 0.f, 1.f));
        owner.repaint();
    }
    void absolute(juce::Point<float> local) {
        const float x = local.x + getX(), y = local.y + getY();
        if (type == Kind::lane)
            put(primary.convertTo0to1(float(tappy::position(laneAt(x), owner.value("sync") > .5f))));
        else if (type == Kind::feedback)
            put((342.063f - y) / 251.517f);
        else
            put((std::atan2(y - cy, x - cx) / speedArcHalfAngle + 1.f) * .5f);
    }
    void step(int sign, bool fine) {
        begin();
        if (type == Kind::os2 || type == Kind::os4)
            put(sign > 0 ? primary.convertTo0to1(float(choice())) : 0.f);
        else if (type == Kind::toggle)
            put(sign > 0 ? 1.f : 0.f);
        else if (type == Kind::lane && owner.value("sync") > .5f) {
            double next = tappy::position(primary.convertFrom0to1(primary.getValue()), true) + sign;
            put(primary.convertTo0to1(float(std::clamp(next, -double(tappy::stops), double(tappy::stops)))));
        } else if (type == Kind::speed && owner.value("sync") > .5f)
            put(float(owner.speedSetting() + double(sign) / tappy::speedIntervals));
        else
            put(currentValue() + sign * (fine ? .001f : .01f));
        end();
    }
};

TappyEditor::TappyEditor(TappyProcessor &p) : AudioProcessorEditor(p), proc(p) {
    setOpaque(true);
    print = juce::Drawable::createFromImageData(BinaryData::CrossFeed_Print_svg, BinaryData::CrossFeed_Print_svgSize);
    strip = juce::Drawable::createFromImageData(BinaryData::ReelTape_svg, BinaryData::ReelTape_svgSize);
    addAndMakeVisible(surface);
    surface.setInterceptsMouseClicks(false, true);
    surface.setBounds(0, 0, 1208, 433);
    add("speed", Kind::speed, "Reel speed", {750, 64, 370, 306});
    add("tapePressure", Kind::reel, "Tape Pressure", {750, 64, 310, 306});
    add("input", Kind::knob, "Record input", {1021, 29, 72, 73});
    add("mix", Kind::knob, "Dry / wet", {1021, 332, 72, 70});
    add("feedback", Kind::feedback, "Feedback", {1120, 78, 67, 283});
    add("feedbackLP", Kind::knob, "Feedback LP", {1109, 370, 40, 46});
    add("crossFeedback", Kind::knob, "Cross-feedback", {1150, 370, 41, 46});
    add("oversampling", Kind::os2, "2x oversampling", {1104, 26, 29, 16});
    add("oversampling", Kind::os4, "4x oversampling", {1104, 42, 29, 17});
    add("bypass", Kind::toggle, "Bypass", {288, 20, 59, 43});
    add("sync", Kind::toggle, "Snap to divisions", {1135, 12, 45, 57});
    for (int i = 0; i < tappy::heads; ++i) {
        const auto laneBounds = juce::Rectangle<float>::leftTopRightBottom(
            tickX.front() - 20, laneY[i] - 21, tickX.back() + 20, laneY[i] + 37);
        add(TappyProcessor::laneId(i), Kind::lane, "Head " + juce::String(i + 1), laneBounds.getSmallestIntegerContainer());
        add(TappyProcessor::enableId(i), Kind::toggle, "Tap " + juce::String(i + 1) + " active",
            {int(51 + 83.839 * i), 374, 49, 45});
    }
    getConstrainer()->setFixedAspectRatio(W / H);
    setResizeLimits(480, juce::roundToInt(480 * H / W), 2172, juce::roundToInt(2172 * H / W));
    setResizable(true, true);
    resizableCorner->setAlpha(0.f); // Keep the corner draggable without painting over the panel.
    constexpr int defaultWidth = 1080;
    setSize(defaultWidth, juce::roundToInt(defaultWidth * H / W));
    startTimerHz(50);
}
TappyEditor::~TappyEditor() { stopTimer(); }
void TappyEditor::presetMenu() {
    if (proc.isTransportPlaying()) return;
    juce::PopupMenu menu;
    menu.addItem(1, "Load preset...");
    menu.addItem(2, "Save preset as...");
    menu.showMenuAsync(juce::PopupMenu::Options().withParentComponent(this),
        [self = juce::Component::SafePointer<TappyEditor>(this)](int action) {
            if (self && action != 0 && !self->proc.isTransportPlaying()) self->browsePreset(action == 2);
        });
}
void TappyEditor::browsePreset(bool save) {
    if (chooser != nullptr)
        return;
    const auto folder = lastFolder.isDirectory() ? lastFolder
        : juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
    chooser = std::make_unique<juce::FileChooser>(save ? "Save Tappy Too preset" : "Load Tappy Too preset",
        save ? folder.getChildFile(juce::File::createLegalFileName(proc.presetName().isEmpty() ? "Untitled" : proc.presetName()) + ".vstpreset") : folder,
        "*.vstpreset", true, false, this);
    using Browser = juce::FileBrowserComponent;
    const int mode = save ? Browser::saveMode | Browser::warnAboutOverwriting : Browser::openMode;
    chooser->launchAsync(mode | Browser::canSelectFiles,
        [self = juce::Component::SafePointer<TappyEditor>(this), save](const juce::FileChooser &dialog) {
            if (!self) return;
            auto file = dialog.getResult();
            auto finished = std::move(self->chooser);
            if (file == juce::File{}) return;
            if (save && file.getFileExtension().isEmpty()) file = file.withFileExtension("vstpreset");
            const auto result = save ? self->proc.savePreset(file) : self->proc.loadPreset(file);
            if (result.wasOk()) {
                self->lastFolder = file.getParentDirectory();
                self->repaint();
            } else {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                    "Tappy Too preset", result.getErrorMessage(), "OK", self.getComponent());
            }
        });
}
void TappyEditor::drawReelLabel(juce::Graphics &g, float scale, double turns) {
    // The strip in the Illustrator print is 80 by 30, tilted up the left reel spoke.
    constexpr float x = 803.5f, y = 155.0f, tilt = -pi / 3.f;
    constexpr int w = 88, h = 34;
    const int pixels = std::max(1, juce::roundToInt(w * scale));
    const unsigned revision = proc.presetRevision();
    if (labelPixels != pixels || revision != labelRevision) {
        labelRevision = revision;
        labelPixels = pixels;
        const auto name = proc.presetName();
        if (name.isEmpty()) {
            labelCache = {};
            return;
        }
        labelCache = juce::Image(juce::Image::ARGB, pixels, std::max(1, juce::roundToInt(h * scale)), true);
        juce::Graphics ink(labelCache);
        ink.addTransform(juce::AffineTransform::scale(float(pixels) / w, float(labelCache.getHeight()) / h));
        if (strip)
            strip->draw(ink, 1.f, juce::AffineTransform::translation(-x, -y)
                .rotated(-tilt).translated(w * .5f, h * .5f));
        juce::GlyphArrangement letters;
        letters.addLineOfText(juce::Font(juce::FontOptions("Segoe Print", 17.f, juce::Font::plain)),
                              name, 0, 0);
        juce::Path handwriting;
        letters.createPath(handwriting);
        ink.setColour(juce::Colour(0xff282420));
        ink.fillPath(handwriting, handwriting.getTransformToScaleToFit(8, 8, w - 16, h - 16, true));
    }
    if (!labelCache.isValid()) return;
    juce::Graphics::ScopedSaveState scoped(g);
    g.addTransform(juce::AffineTransform::translation(-w * .5f, -h * .5f)
        .rotated(tilt).translated(x, y).rotated(float(2 * pi * turns), cx, cy));
    g.drawImage(labelCache, {0.f, 0.f, float(w), float(h)}, juce::RectanglePlacement::stretchToFit);
}
void TappyEditor::add(const juce::String &id, Kind kind, const juce::String &name,
                      juce::Rectangle<int> bounds) {
    auto c = std::make_unique<Control>(*this, id, kind, name);
    c->setBounds(bounds);
    surface.addAndMakeVisible(*c);
    handles.push_back(std::move(c));
}
void TappyEditor::resized() {
    surface.setTransform(juce::RectanglePlacement().getTransformToFit(panelBounds, getLocalBounds().toFloat()));
}
void TappyEditor::timerCallback() {
    if (!isShowing())
        return;
    bool controlsChanged = false;
    for (auto &control : handles)
        controlsChanged |= control->valueChangedSinceLastFrame();

    const double turns = proc.displayClock().turns;
    if (controlsChanged) {
        repaint();
    } else {
        const auto invalidate = [&](juce::Rectangle<float> area) {
            repaint(area.transformedBy(surface.getTransform()).getSmallestIntegerContainer().expanded(1));
        };
        if (turns != lastReelTurns || labelRevision != proc.presetRevision())
            invalidate(reelAnimationBounds);
        if (value("sync") > .5f)
            invalidate(syncLedBounds);
    }
    lastReelTurns = turns;
}
void TappyEditor::paint(juce::Graphics &g) {
    g.fillAll(panelColour);
    const auto transform = surface.getTransform();
    const float scale = g.getInternalContext().getPhysicalPixelScaleFactor();
    const int cacheWidth = std::clamp(juce::roundToInt(W * transform.mat00 * scale), 1, 4096);
    const int cacheHeight = std::max(1, juce::roundToInt(cacheWidth * H / W));
    if (printCache.getWidth() != cacheWidth || printCache.getHeight() != cacheHeight) {
        printCache = juce::Image(juce::Image::RGB, cacheWidth, cacheHeight, false);
        juce::Graphics background(printCache);
        background.fillAll(panelColour);
        background.addTransform(juce::AffineTransform::scale(cacheWidth / W, cacheHeight / H));
        if (print)
            print->draw(background, 1);
    }
    g.addTransform(transform);
    g.drawImage(printCache, panelBounds, juce::RectanglePlacement::stretchToFit);
    const auto clock = proc.displayClock();
    for (int hole = 0; hole < 3; ++hole) {
        const float a = float(2 * pi * (clock.turns + hole / 3.)) - .5f * pi;
        disc(g, cx + 80.485f * std::cos(a), cy + 80.485f * std::sin(a), 60.364f, juce::Colour(0xff0b090c));
    }
    drawReelLabel(g, cacheWidth / W, clock.turns);
    const float angle = (2 * float(speedSetting()) - 1) * speedArcHalfAngle;
    const juce::Point<float> dir{std::cos(angle), std::sin(angle)}, center{cx, cy};
    const auto start = center + dir * 151.f, end = center + dir * 176.f;
    g.setColour(dark);
    g.drawLine({start, end}, 10.8f);
    g.setColour(juce::Colour(0xff898989));
    g.drawLine({start, end}, 8.4f);
    disc(g, end.x, end.y, 5.3f, dark);
    disc(g, end.x, end.y, 4.6f, juce::Colour(0xfff9f578));
    auto pointer = [&](float x, float y, float normal) {
        const float a = pi * (.75f + 1.5f * normal);
        g.setColour(juce::Colours::white);
        juce::Path line;
        line.startNewSubPath(x, y);
        line.lineTo(x + 27 * std::cos(a), y + 27 * std::sin(a));
        g.strokePath(line,
                     juce::PathStrokeType(4.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    };
    pointer(1056.372f, 65.394f, (value("input") + 24) / 48);
    pointer(1056.372f, 367.215f, value("mix"));
    const auto returnPointer = [&](float x, const char *id) {
        const float angle = pi * (.75f + 1.5f * proc.state.getParameter(id)->getValue());
        const juce::Point<float> centre{x, 386.079f};
        g.setColour(juce::Colours::white);
        g.drawLine({centre, centre + juce::Point<float>(std::cos(angle), std::sin(angle)) * 13.3f}, 1.4f);
    };
    returnPointer(1129.558f, "feedbackLP");
    returnPointer(1169.801f, "crossFeedback");
    const juce::Rectangle<float> readout{1104, 402, 51, 15};
    g.setColour(cream);
    g.setFont(juce::FontOptions(11.5f));
    g.drawText(TappyProcessor::frequencyText(value("feedbackLP")), readout, juce::Justification::centred);
    const int os = int(value("oversampling"));
    const juce::Colour unlit(0xff03090b);
    disc(g, 1126.797f, 33.7f, 2.515f, os == 1 ? salmon : unlit);
    disc(g, 1126.743f, 50.3f, 2.515f, os == 2 ? salmon : unlit);
    const float fy = 342.063f - value("feedback") * 251.517f;
    triangle(g, {1156.98f, fy}, {1172.07f, fy - 10.06f}, {1172.07f, fy + 10.06f}, salmon);
    const bool sync = value("sync") > .5f;
    const float pulse = sync ? float(std::exp(-7. * clock.beat)) : 0.f;
    disc(g, 1156.979f, 35.212f, 10.6f, dark);
    disc(g, 1156.979f, 35.212f, 9.9f, olive);
    disc(g, 1156.979f, 50.3f, 2.65f, ledOff.interpolatedWith(ledRed, pulse));
    const bool bypassed = value("bypass") > .5f;
    toggle(g, 316.912f, 35.267f, !bypassed);
    for (int h = 0; h < tappy::heads; ++h) {
        const bool on = value(TappyProcessor::enableId(h)) > .5f;
        const float x = headX(tappy::position(value(TappyProcessor::laneId(h)), sync)),
                    y = laneY[h];
        triangle(g, {x, y}, {x - 10.06f, y - 15.091f}, {x + 10.06f, y - 15.091f}, on ? olive : salmon);
        toggle(g, 75.455f + 83.839f * h, 387.336f, on);
    }
}
