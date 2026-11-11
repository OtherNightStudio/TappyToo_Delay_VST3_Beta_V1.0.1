#include "TapeEngine.h"

namespace tappy {
namespace {
constexpr double pi = 3.14159265358979323846;
double gain(double db) { return std::pow(10., db / 20.); }
} // namespace

void TapeEngine::prepare(double sampleRate) {
    fs = std::max(8000., sampleRate);
    smoothing = -std::expm1(-1. / (.006 * fs));
    motorSmoothing = -std::expm1(-1. / (.022 * fs));
    dcPole = std::exp(-2 * pi * 5 / fs);
    cutoffSmoothing = -std::expm1(-32. / (.015 * fs));
    fadeSamples = std::max(1, int(std::round(.004 * fs)));
    const auto design = [](auto &fir, int factor) {
        const int order = int(fir.size()) - 1;
        const double cutoff = .94 / factor;
        double sum = 0;
        for (int i = 0; i <= order / 2; ++i) {
            const double n = i - order / 2;
            const double window = .42 - .5 * std::cos(2 * pi * i / order) + .08 * std::cos(4 * pi * i / order);
            fir[i] = fir[order - i] = window * (n == 0 ? cutoff : std::sin(cutoff * pi * n) / (pi * n));
            sum += (n == 0 ? 1. : 2.) * fir[i];
        }
        for (auto &v : fir)
            v /= sum;
    };
    design(fir2, 2);
    design(fir4, 4);
    reset();
}
void TapeEngine::reset() {
    tape.assign(size, Cell{});
    previous = {};
    for (auto *pair : {&tracks, &returns})
        for (auto &t : *pair) {
            t.record = {};
            t.emphasis = t.playback = t.bandwidth = {};
            t.dcX = t.dcY = 0;
        }
    dryDelay = {};
    phase = reelPhase = 0;
    write = dryIndex = toneCounter = 0;
    lastToneRate = returnCutoff = -1;
    activeSlot = transition = 0;
    first = true;
}
namespace {
float saturate(double signal, double knee) {
    const double excess = std::abs(signal) - knee;
    if (excess <= 0)
        return static_cast<float>(signal);
    const double shoulder = .65 * knee;
    const double magnitude = knee + shoulder * std::tanh(excess / shoulder);
    return static_cast<float>(std::copysign(magnitude, signal));
}
}

float TapeEngine::RecordHead::native(float x, double knee) {
    const float delayed = input[slot];
    input[slot] = saturate(x, knee);
    slot = (slot + 1) % latency;
    return delayed;
}

template<int factor>
float TapeEngine::RecordHead::process(float x, double k, const Fir<factor> &taps) {
    if (x == 0.f) {
        if (consecutiveZeros == flushSamples)
            return 0.f;
        ++consecutiveZeros;
    } else {
        consecutiveZeros = 0;
    }

    input[slot] = input[slot + frames] = x;
    const auto *history = input.data() + slot;
    constexpr int order = factor * latency;
    float *frame = output.data() + factor * slot;
    for (int subphase = 0; subphase < factor; ++subphase) {
        double up = 0.;
        if (subphase == 0 || subphase == factor / 2) {
            const int last = subphase == 0 ? latency : latency - 1;
            if (subphase == 0)
                up = taps[order / 2] * history[latency / 2];
            for (int i = 0; i < latency / 2; ++i)
                up += taps[factor * i + subphase] * (double(history[i]) + history[last - i]);
        } else {
            for (int i = 0; i < latency; ++i)
                up += taps[factor * i + subphase] * history[i];
        }
        const int index = factor - 1 - subphase;
        frame[index] = frame[index + factor * frames] = saturate(factor * up, k);
    }
    // Phase zero aligns every decimator to the same host sample.
    const auto *downHistory = frame + factor - 1;
    double down = taps[order / 2] * downHistory[order / 2];
    for (int i = 0; i < order / 2; ++i)
        down += taps[i] * (double(downHistory[i]) + downHistory[order - i]);
    slot = slot == 0 ? frames - 1 : slot - 1;
    return static_cast<float>(down);
}
void TapeEngine::setTone(double r) {
    if (std::abs(r - lastToneRate) < .0001)
        return;
    lastToneRate = r;
    // At 120 BPM, full speed is the 7.5 ips voicing. Slowing the transport
    // brings the high-frequency shoulder down sooner than the broadband knee.
    const double octaves = std::clamp(std::log2(std::max(r, .0625)), -4., 2.);
    knee = .72 * gain(1.1 * octaves);
    const double shelfDb = std::clamp(1.5 - 6.5 * octaves, .5, 10.);
    const double A = std::pow(10., shelfDb / 40.);
    const double corner = std::clamp(3000. * std::sqrt(std::max(.0625, r)), 900., fs * .22);
    const double cosine = std::cos(2 * pi * corner / fs), sine = std::sin(2 * pi * corner / fs);
    const double beta = std::sqrt(2 * A) * sine;
    const double u = A + 1, v = A - 1;
    const double b0 = A * (u + v * cosine + beta), b1 = -2 * A * (v + u * cosine),
                 b2 = A * (u + v * cosine - beta);
    const double a0 = u - v * cosine + beta, a1 = 2 * (v - u * cosine), a2 = u - v * cosine - beta;
    for (auto *pair : {&tracks, &returns}) {
        for (auto &t : *pair) {
            t.emphasis.set(b0, b1, b2, a0, a1, a2);
            t.playback.set(a0, a1, a2, b0, b1, b2);
        }
    }
    setBandwidth(tracks, 21000.);
}
void TapeEngine::setBandwidth(std::array<Track, 2> &pair, double referenceHz) {
    const double r = lastToneRate;
    const double cutoff = std::clamp(std::min(referenceHz * std::pow(std::max(r, .001), .48),
                                              referenceRate * r * .40), 20., fs * .455);
    const double w = 2 * pi * cutoff / fs, c = std::cos(w), alpha = std::sin(w) / std::sqrt(2.);
    for (auto &t : pair)
        t.bandwidth.set((1 - c) * .5, 1 - c, (1 - c) * .5, 1 + alpha, -2 * c, 1 - alpha);
}
void TapeEngine::updateRecordMode(int requested) {
    if (transition == 0) {
        if (requested == activeFactor)
            return;
        nextFactor = requested;
        for (auto *pair : {&tracks, &returns})
            for (auto &t : *pair)
                t.record[1 - activeSlot] = {};
    }
    ++transition;
    // Fill both FIR histories before bringing the new record path into the mix.
    fade = std::clamp(float(transition - RecordHead::flushSamples) / fadeSamples, 0.f, 1.f);
    if (fade >= 1.f) {
        activeSlot = 1 - activeSlot;
        activeFactor = nextFactor;
        transition = 0;
    }
}
float TapeEngine::record(Track &t, double input) {
    const float x = t.emphasis.tick(input);
    const auto process = [&](RecordHead &head, int factor) {
        if (factor == 2) return head.process<2>(x, knee, fir2);
        if (factor == 4) return head.process<4>(x, knee, fir4);
        return head.native(x, knee);
    };
    float y = process(t.record[activeSlot], activeFactor);
    if (transition != 0)
        y += fade * (process(t.record[1 - activeSlot], nextFactor) - y);
    return t.playback.tick(y);
}
float TapeEngine::toTape(Track &t, float recorded) {
    const float highpass = static_cast<float>(recorded - t.dcX + dcPole * t.dcY);
    t.dcX = recorded;
    t.dcY = highpass;
    return t.bandwidth.tick(highpass);
}
TapeEngine::Cell TapeEngine::read(int i, float f) const {
    if (f == 0.f)
        return tape[i & mask];
    const auto &left = tape[(i - 1) & mask], &start = tape[i & mask];
    const auto &end = tape[(i + 1) & mask], &right = tape[(i + 2) & mask];
    Cell cell;
    for (size_t ch = 0; ch < cell.size(); ++ch) {
        const float a = left[ch], b = start[ch], c = end[ch], d = right[ch];
        cell[ch] = std::clamp(b + .5f * f * ((c - a) + f * ((2 * a - 5 * b + 4 * c - d) + f * (3 * (b - c) + d - a))),
                              std::min(b, c), std::max(b, c));
    }
    return cell;
}
void TapeEngine::process(float *left, float *right, int samples, const Controls &p) {
    double target[heads];
    for (int h = 0; h < heads; ++h)
        target[h] = position(p.lane[h], p.sync);
    const double rateTarget = std::clamp(p.bpm, 20., 400.) / 120. * p.loadedSpeed();
    const double inputTarget = gain(p.inputDb), fbTarget = feedbackGain(p.feedback);
    if (first) {
        activeFactor = p.oversampling;
        feedbackLP = p.feedbackLP;
        rate = rateTarget;
        drive = inputTarget;
        mix = p.mix;
        feedback = fbTarget;
        crossFeedback = p.crossFeedback;
        bypass = p.bypass;
        moving = rateTarget > 0;
        for (int h = 0; h < heads; ++h) {
            lane[h] = target[h];
            active[h] = p.enabled[h];
        }
        first = false;
    }
    struct TapRead {
        double distance, weight, stereo, left, right;
    };
    std::array<TapRead, heads> taps;
    int numTaps = 0;
    double sideWeight[2]{}, delayedWeight = 0., centerWeight = 0.;
    const auto updateTapMix = [&] {
        numTaps = 0;
        sideWeight[0] = sideWeight[1] = delayedWeight = centerWeight = 0.;
        for (int h = 0; h < heads; ++h) {
            const double weight = active[h];
            if (weight < .000001)
                continue;
            const double cells = 2. * referenceRate * noteLength(lane[h]);
            const double delayed = std::min(1., cells / 8.);
            centerWeight += weight * (1. - delayed);
            if (delayed == 0.)
                continue;
            const double pan = std::clamp(lane[h], -1., 1.);
            auto &tap = taps[numTaps++];
            tap = {std::max(2., cells), weight * delayed, 1. - std::abs(pan),
                   std::max(0., -pan), std::max(0., pan)};
            sideWeight[0] += tap.weight * (1. - tap.right);
            sideWeight[1] += tap.weight * (1. - tap.left);
            delayedWeight += tap.weight;
        }
        for (double &weight : sideWeight)
            weight = 1. / std::max(1., weight);
        delayedWeight = 1. / std::max(1., delayedWeight);
        centerWeight = std::min(1., centerWeight);
    };
    // Finish sub-audible ramps at the block boundary so settled heads reuse their mix.
    bool headsMoving = false;
    for (int h = 0; h < heads; ++h) {
        if (std::abs(target[h] - lane[h]) < 1.e-9) lane[h] = target[h];
        if (std::abs(double(p.enabled[h]) - active[h]) < 1.e-8) active[h] = p.enabled[h];
        headsMoving |= lane[h] != target[h] || active[h] != double(p.enabled[h]);
    }
    if (!headsMoving)
        updateTapMix();
    if (std::abs(fbTarget - feedback) < 1.e-12)
        feedback = fbTarget;
    const bool feedbackMoving = feedback != fbTarget;
    if (std::abs(crossFeedback - p.crossFeedback) < 1.e-9) crossFeedback = p.crossFeedback;
    const bool crossMoving = crossFeedback != p.crossFeedback;
    double returnGain, returnTrim;
    const auto updateReturnGain = [&] {
        // Ease out of drive compensation above unity so the loop can reach tape level.
        const double loopDrive = 1. + 1.5 * feedback * feedback * std::clamp((1.6 - feedback) / .6, 0., 1.);
        returnGain = feedback * loopDrive;
        returnTrim = 1. / loopDrive;
    };
    updateReturnGain();
    for (int n = 0; n < samples; ++n) {
        rate += motorSmoothing * (rateTarget - rate);
        if (rateTarget == 0 && rate < .0001)
            rate = 0;
        if (toneCounter++ % 32 == 0) {
            const bool rateChanged = std::abs(rate - lastToneRate) >= .0001;
            setTone(rate);
            feedbackLP += cutoffSmoothing * (p.feedbackLP - feedbackLP);
            if (rateChanged || std::abs(feedbackLP - returnCutoff) > .01) {
                setBandwidth(returns, feedbackLP);
                returnCutoff = feedbackLP;
            }
        }
        if (transition != 0 || p.oversampling != activeFactor)
            updateRecordMode(p.oversampling);
        if (toneCounter == 32000)
            toneCounter = 0;
        drive += smoothing * (inputTarget - drive);
        mix += smoothing * (p.mix - mix);
        if (feedbackMoving) {
            feedback += smoothing * (fbTarget - feedback);
            updateReturnGain();
        }
        if (crossMoving) crossFeedback += smoothing * (p.crossFeedback - crossFeedback);
        bypass += smoothing * (double(p.bypass) - bypass);
        moving += smoothing * (std::min(1., rate / .012) - moving);
        const double advance = referenceRate * rate / fs;
        reelPhase += rate * (2. / 3.) / fs;
        if (reelPhase >= 1.)
            reelPhase -= 1.;
        if (headsMoving) {
            for (int h = 0; h < heads; ++h) {
                lane[h] += smoothing * (target[h] - lane[h]);
                active[h] += smoothing * (double(p.enabled[h]) - active[h]);
            }
        }
        if (headsMoving) updateTapMix();
        double firstEcho[2]{}, repeat[2]{};
        for (int h = 0; h < numTaps; ++h) {
            const auto &tap = taps[h];
            const double location = write - 1 + phase - tap.distance;
            const int index = int(std::floor(location));
            const float fraction = static_cast<float>(location - index);
            const auto cell = read(index, fraction);
            const double l = cell[0], r = cell[1];
            const double mid = .5 * (l + r);
            firstEcho[0] += tap.weight * (l * tap.stereo + mid * tap.left);
            firstEcho[1] += tap.weight * (r * tap.stereo + mid * tap.right);
            // The return has already been panned by the loop. Reading it in
            // stereo lets even a single hard-left head throw the next echo right.
            repeat[0] += tap.weight * cell[2];
            repeat[1] += tap.weight * cell[3];
        }
        const float input[2] = {std::isfinite(left[n]) ? left[n] : 0.f,
                                right && std::isfinite(right[n])
                                    ? right[n]
                                    : (right ? 0.f : (std::isfinite(left[n]) ? left[n] : 0.f))};
        double echoes[2]{};
        for (int ch = 0; ch < 2; ++ch)
            echoes[ch] = firstEcho[ch] * sideWeight[ch] + repeat[ch] * delayedWeight;
        const double monitor = centerWeight * moving;
        // Unity direct level through the first half of travel. A centred
        // record monitor replaces the corresponding dry share at the same latency.
        const double directGain = std::min(2. * (1. - mix), 1. - mix * monitor * std::min(1., drive));
        Cell next;
        float result[2]{};
        for (int ch = 0; ch < 2; ++ch) {
            const float source = record(tracks[ch], drive * input[ch] * (1. - bypass));
            next[ch] = toTape(tracks[ch], source);
            const double cross = (1. - crossFeedback) * echoes[ch] + crossFeedback * echoes[1 - ch];
            const float regen = record(returns[ch], (1. - bypass) * returnGain * cross);
            next[2 + ch] = toTape(returns[ch], static_cast<float>(regen * returnTrim));
            const double wet = moving * echoes[ch] + monitor * source;
            const float dry = dryDelay[ch][dryIndex];
            dryDelay[ch][dryIndex] = input[ch];
            const double effected = directGain * dry + mix * wet;
            result[ch] = static_cast<float>(dry + (1. - bypass) * (effected - dry));
        }
        const double old = phase;
        phase += advance;
        for (int k = 1; phase >= 1.; ++k) {
            const double interp = (k - old) / advance;
            for (size_t ch = 0; ch < next.size(); ++ch)
                tape[write][ch] = static_cast<float>(previous[ch] + interp * (next[ch] - previous[ch]));
            write = (write + 1) & mask;
            phase -= 1.;
        }
        previous = next;
        dryIndex = (dryIndex + 1) % latency;
        left[n] = right ? result[0] : .5f * (result[0] + result[1]);
        if (right)
            right[n] = result[1];
    }
}
} // namespace tappy
