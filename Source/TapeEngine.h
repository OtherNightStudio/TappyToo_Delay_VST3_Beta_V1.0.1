#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace tappy {
constexpr int heads = 4;
constexpr int stops = 13;
constexpr std::array<double, stops + 1> fractions{0,       1. / 64, 3. / 128, 1. / 32, 3. / 64,
                                                  1. / 16, 3. / 32, 1. / 8,   3. / 16, 1. / 4,
                                                  3. / 8,  1. / 2,  3. / 4,   1.};
constexpr std::array<const char *, stops + 1> labels{"0",   "1/64", "3/128", "1/32", "3/64", "1/16", "3/32",
                                                     "1/8", "3/16", "1/4",   "3/8",  "1/2",  "3/4",  "1"};
constexpr int speedIntervals = 16;
inline double reelSpeed(double raw, bool snap) {
    raw = std::clamp(raw, 0., 1.);
    return snap ? std::round(raw * speedIntervals) / speedIntervals : raw;
}
inline double position(double raw, bool snap) {
    raw = std::clamp(raw, -double(stops), double(stops));
    return snap ? std::round(raw) : raw;
}
inline double noteLength(double lane) {
    const auto x = std::min(std::abs(lane), double(stops));
    const auto i = std::min(int(x), stops - 1);
    return fractions[i] + (x - i) * (fractions[i + 1] - fractions[i]);
}
constexpr int maxTapePressure = 127;
struct Controls {
    std::array<double, heads> lane{};
    std::array<bool, heads> enabled{true, true, true, true};
    double bpm = 120, speed = 1, inputDb = 0, feedback = .35, mix = .35;
    double feedbackLP = 12500.;
    int oversampling = 2;
    int tapePressure = 0;
    double crossFeedback = 0.;
    bool sync = false, bypass = false;
    double loadedSpeed() const {
        return reelSpeed(speed, sync) * (1. - double(tapePressure) / maxTapePressure);
    }
};
inline double feedbackGain(double amount) {
    if (amount >= .9)
        return 1. + 6. * (amount - .9);
    const double approach = std::max(0., (amount - .8) * 10.);
    return .975 * amount + .1225 * approach * approach;
}

class TapeEngine {
  public:
    static constexpr int latency = 16; // host samples in every record mode
    void prepare(double sampleRate);
    void reset();
    void process(float *left, float *right, int samples, const Controls &);
    double turns() const { return reelPhase; }
    double transportRatio() const { return rate; }

  private:
    template<int factor> using Fir = std::array<double, factor * latency + 1>;
    struct RecordHead {
        // Mirrored history lets both FIRs read consecutive samples across the wrap.
        static constexpr int frames = latency + 1;
        static constexpr int flushSamples = 2 * (frames - 1) + 1;
        std::array<float, 2 * frames> input{};
        std::array<float, 2 * frames * 4> output{};
        int slot = 0, consecutiveZeros = flushSamples;
        template<int factor> float process(float, double, const Fir<factor> &);
        float native(float, double);
    };
    struct Filter {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void set(double n0, double n1, double n2, double d0, double d1, double d2) {
            b0 = n0 / d0;
            b1 = n1 / d0;
            b2 = n2 / d0;
            a1 = d1 / d0;
            a2 = d2 / d0;
        }
        float tick(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return static_cast<float>(y);
        }
    };
    struct Track {
        std::array<RecordHead, 2> record;
        Filter emphasis, playback, bandwidth;
        float dcX = 0, dcY = 0;
    };
    // Spatial samples per second at 7.5 ips. One whole note at the reference tempo occupies 192000 cells.
    static constexpr double referenceRate = 96000.;
    static constexpr int size = 262144, mask = size - 1;
    double fs = 48000, smoothing = 0, motorSmoothing = 0, dcPole = 0, phase = 0, rate = 1, reelPhase = 0,
           lastToneRate = -1, knee = .72, returnCutoff = -1, feedbackLP = 12500., cutoffSmoothing = 0.;
    double drive = 1, feedback = 0, mix = 0, bypass = 0, moving = 1;
    double crossFeedback = 0.;
    std::array<double, heads> active{}, lane{};
    Fir<2> fir2{};
    Fir<4> fir4{};
    int activeSlot = 0, activeFactor = 2, nextFactor = 2, transition = 0, fadeSamples = 0;
    float fade = 0;
    std::array<Track, 2> tracks, returns;
    using Cell = std::array<float, 4>; // stereo recording followed by stereo return
    std::vector<Cell> tape;
    Cell previous{};
    std::array<std::array<float, latency>, 2> dryDelay{};
    int write = 0, dryIndex = 0, toneCounter = 0;
    bool first = true;
    void setTone(double);
    void setBandwidth(std::array<Track, 2> &, double);
    void updateRecordMode(int requested);
    Cell read(int index, float fraction) const;
    float record(Track &, double input);
    float toTape(Track &, float recorded);
};
} // namespace tappy
