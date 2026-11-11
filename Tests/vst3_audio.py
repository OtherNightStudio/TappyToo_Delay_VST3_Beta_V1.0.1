"""Offline checks of the compiled Windows VST3 using Pedalboard and NumPy."""
import argparse
from pathlib import Path

import numpy as np
from pedalboard import VST3Plugin


def open_plugin(bundle):
    module = bundle / "Contents" / "x86_64-win" / "TappyToo.vst3"
    effect = VST3Plugin(str(module.resolve()))
    assert effect.name == "Tappy Too Stereo 4Tap Delay"
    assert effect.manufacturer_name == "OtherNightStudio"
    return effect


def place(effect, head, position):
    effect.parameters[f"head_{head}_position"].raw_value = (position + 13) / 26
    setattr(effect, f"tap_{head}_active", True)


def impulse_test(bundle, rate, speed, sync):
    p = open_plugin(bundle)
    p.dry_wet = 1
    p.feedback = 0
    p.reel_speed = speed
    p.sync_heads_to_divisions = sync
    p.tap_3_active = p.tap_4_active = False
    place(p, 1, -7.25)
    place(p, 2, 8)
    transport = np.floor(speed * 16 + 0.5) / 16 if sync else speed
    source = np.zeros((2, int(rate / transport)), dtype=np.float32)
    source[:, 256] = 0.025
    expected = ((0.25 if sync else 0.28125) / transport, 0.375 / transport)
    output = p(source, rate, buffer_size=127)
    for channel, time in enumerate(expected):
        peak = np.argmax(abs(output[channel]))
        assert abs((peak - 256) / rate - time) < 0.0002, (rate, speed, sync, peak, time)
        assert abs(output[channel, peak]) > 0.001
        assert abs(output[1 - channel, peak]) < 0.0001


def tone(bundle, speed, hz, amplitude):
    rate = 48000
    p = open_plugin(bundle)
    p.tap_2_active = p.tap_3_active = p.tap_4_active = False
    p.dry_wet = 1
    p.reel_speed = speed
    t = np.arange(rate) / rate
    x = amplitude * np.sin(2 * np.pi * hz * t)
    y = p(np.stack((x, x)), rate)[0, rate // 2:]
    # Window the settled second half to reject DC and leakage in the 333 Hz test.
    window = np.hanning(len(y))
    def magnitude(f):
        return 2 * abs(np.dot(window * y, np.exp(-2j * np.pi * f * t[:len(y)]))) / window.sum()
    fundamental = magnitude(hz)
    return 20 * np.log10(fundamental), 100 * magnitude(3 * hz) / fundamental


def direct_gain_and_cross_feedback(bundle):
    rate = 48000
    t = np.arange(4800) / rate
    dry = np.stack((0.2 * np.sin(2 * np.pi * 750 * t), 0.15 * np.cos(2 * np.pi * 500 * t)))
    for lane in (0, -7):
        for trim in (-24, 0):
            p = open_plugin(bundle)
            p.dry_wet = .35
            p.record_input_db = trim
            for head in range(1, 5):
                place(p, head, lane)
            output = p(dry, rate)
            np.testing.assert_allclose(output[:, 256:-64], dry[:, 256:-64], atol=5e-5)
    for sign, first_side in ((-1, 0), (1, 1)):
        p = open_plugin(bundle)
        p.dry_wet = 1
        p.feedback = .8
        p.tap_2_active = p.tap_3_active = p.tap_4_active = False
        place(p, 1, sign * 5)
        source = np.zeros((2, rate), dtype=np.float32)
        source[:, 128] = .15
        other = 1 - first_side
        original = None
        for cross in (0., .25, .5, .85, 1.):
            p.parameters["cross_feedback"].raw_value = cross
            output = p(source, rate)
            first = output[:, 6080:6312]
            if original is None:
                original = first.copy()
            np.testing.assert_array_equal(first, original)
            assert np.max(abs(first[first_side])) > .05 and np.max(abs(first[other])) < 1e-6
            second = np.max(abs(output[:, 12120:12312]), axis=1)
            assert abs(second[other] / sum(second) - cross) < .0001, (cross, second)
            if cross == 0: straight = sum(second)
            assert abs(sum(second) - straight) < .00001
        p.feedback = 0
        output = p(source, rate)
        assert abs(output[:, 12120:12312]).max() < .00001
    print("PASS: unchanged first tap, proportionate cross-routing at five settings, zero fader kills returns")


def feedback_treble(bundle):
    rate = 48000
    for speed in (1, .5):
        p = open_plugin(bundle)
        p.dry_wet = 1
        p.feedback = .75
        p.reel_speed = speed
        p.tap_2_active = p.tap_3_active = p.tap_4_active = False
        place(p, 1, -5)
        source = np.zeros((2, int(rate / speed)), dtype=np.float32)
        source[:, 128] = .01
        output = p(source, rate, buffer_size=127)
        levels = []
        for echo in (1, 4):
            start = 128 + round(echo * .125 * rate / speed) - 32
            # Sum the stereo return so the pan of this repeat does not affect the ratio.
            spectrum = abs(np.fft.rfft(output[:, start:start + 1024].sum(axis=0), n=rate))
            levels.append(20 * np.log10(spectrum[10000] / spectrum[1000]))
        first, fourth = levels
        assert abs(first) < .6, (speed, first)
        loss = first - fourth
        lo, hi = (2, 4.5) if speed == 1 else (11, 15)
        assert lo < loss < hi, (speed, loss)
        print(f"10 kHz relative to 1 kHz at {speed}x, first / fourth echo: {first:.2f} / {fourth:.2f} dB")


def record_aliasing(bundle, mode):
    rate, hz = 48000, 9973
    p = open_plugin(bundle)
    p.oversampling = mode
    p.dry_wet = 1
    p.feedback = 0
    p.tap_2_active = p.tap_3_active = p.tap_4_active = False
    t = np.arange(rate * 2) / rate
    x = np.sin(2 * np.pi * hz * t)
    y = p(np.stack((x, x)), rate)[0]
    # A settled one-second window puts every genuine harmonic in a known FFT bin.
    spectrum = abs(np.fft.rfft(y[rate // 2:rate // 2 + rate]))
    fundamental = spectrum[hz]
    spectrum[::hz] = 0
    aliases_db = 20 * np.log10(np.linalg.norm(spectrum) / fundamental)
    print(f"{mode} oversampling, full-scale {hz} Hz sine: non-harmonic energy {aliases_db:.1f} dBc")
    return aliases_db


def feedback_controls(bundle):
    rate = 48000
    def repeats(cutoff, feedback, signal, spacing):
        p = open_plugin(bundle)
        p.dry_wet = 1
        p.feedback = feedback
        p.parameters["feedback_lp"].raw_value = np.log(cutoff / 20) / np.log(1000)
        p.tap_2_active = p.tap_3_active = p.tap_4_active = False
        place(p, 1, spacing)
        return p(signal, rate, buffer_size=127)

    x = np.zeros((2, rate), dtype=np.float32)
    x[:, 128] = .05
    dark, bright = [repeats(hz, .8, x, -5) for hz in (1200, 18000)]
    np.testing.assert_allclose(dark[:, :12000], bright[:, :12000], atol=1e-7)
    def treble(y):
        spec = abs(np.fft.rfft(y[:, 24100:25124].sum(axis=0), n=rate))
        return 20 * np.log10(spec[8000] / spec[700])
    change = treble(bright) - treble(dark)
    assert change > 35, change
    print(f"Fourth echo, opening LP from 1.2 to 18 kHz: +{change:.1f} dB at 8 kHz relative to 700 Hz")

    burst = np.zeros((2, rate * 3), dtype=np.float32)
    t = np.arange(600) / rate
    burst[:, :len(t)] = .08 * np.hanning(len(t)) * np.sin(2 * np.pi * 600 * t)
    for amount, grows in ((.8, False), (.92, True), (1., True)):
        y = repeats(12500, amount, burst, -1)
        assert np.isfinite(y).all() and abs(y).max() < 2
        early = np.max(abs(y[:, :rate//10]))
        late = np.max(abs(y[:, -rate//2:]))
        assert late > early * 2 if grows else late < early * .01, (amount, early, late)
    p = open_plugin(bundle)
    p.dry_wet, p.feedback = 1., 1.
    p.tap_2_active = p.tap_3_active = p.tap_4_active = False
    place(p, 1, -1)
    ringing = p(burst, rate, reset=False, buffer_size=127)
    p.feedback = .84
    decay = p(np.zeros_like(burst), rate, reset=False, buffer_size=127)
    peak = np.max(abs(ringing[:, -rate//2:]))
    assert peak > .35 and np.max(abs(decay[:, :rate//10])) > .2
    ratio = np.max(abs(decay[:, -rate//2:])) / peak
    assert ratio < .001, ratio
    print(f"Established oscillation at full feedback, pulled to 84%: {20*np.log10(ratio):.1f} dB after 3 s")
    p.reel_speed, p.feedback = .0125, 1.
    place(p, 1, -.04)
    burst[:, :len(t)] = .08 * np.hanning(len(t)) * np.sin(2 * np.pi * 125 * t)
    p.reset()
    drone = p(burst, rate, reset=False, buffer_size=127)
    p.feedback = .84
    release = p(np.zeros((2, 6 * rate), dtype=np.float32), rate, reset=False, buffer_size=127)
    sustained = np.max(abs(drone[:, -rate//2:]))
    assert sustained > .025 and np.max(abs(release[:, -rate//2:])) < .01 * sustained, sustained


def tape_pressure_audio(bundle):
    rate = 48000
    p = open_plugin(bundle)
    assert "reel_push_deg" not in p.parameters and p.parameters["tape_pressure"].raw_value == 0
    assert p.parameters["cross_feedback"].raw_value == 0
    p.dry_wet, p.feedback = 1., 0.
    p.tap_2_active = p.tap_3_active = p.tap_4_active = False
    p.sync_heads_to_divisions = True
    place(p, 1, -5)
    source = np.zeros((2, rate), np.float32)
    source[:, 128] = .10
    p(np.zeros_like(source), rate, reset=False, buffer_size=127)
    baseline = p(source, rate, reset=False, buffer_size=127)
    for pressure in (1, 32, 64, 95, 127):
        p.parameters["tape_pressure"].raw_value = pressure / 127
        out = p(source, rate, buffer_size=127)
        if pressure == 127:
            assert abs(out).max() == 0
        else:
            peak = abs(out[0]).argmax()
            expected = 128 + 6000 / (1 - pressure/127)
            assert abs(peak-expected) < 6 and abs(out[0,peak]) > .005, (pressure, peak, expected)
    # Release a running stop, let the motor recover, and feed a new impulse.
    p.parameters["tape_pressure"].raw_value = 0
    p(np.zeros_like(source), rate, reset=False, buffer_size=127)
    resumed = p(source, rate, reset=False, buffer_size=127)
    assert abs(abs(resumed[0]).argmax()-abs(baseline[0]).argmax()) < 6
    assert abs(resumed).max() > .05
    assert p.reel_speed == 1
    # Measure material that was already on the tape before the motor slowed.
    p.reset()
    place(p, 1, -9)
    tone = np.tile(.1 * np.sin(2*np.pi*440*np.arange(rate)/rate), (2, 1)).astype(np.float32)
    silence = np.zeros_like(source)
    p(tone, rate, reset=False, buffer_size=127)
    p.parameters["tape_pressure"].raw_value = 64/127
    braked = p(silence[:, :19200], rate, reset=False, buffer_size=127)
    p.parameters["tape_pressure"].raw_value = 0
    accelerated = p(silence[:, :9600], rate, reset=False, buffer_size=127)
    def pitch(y):
        crossings = np.flatnonzero((y[:-1] <= 0) & (y[1:] > 0))
        assert len(crossings) > 10
        return rate / np.diff(crossings).mean()
    slow, fast = pitch(braked[0, 12000:18000]), pitch(accelerated[0, 6000:9350])
    assert abs(slow - 440*63/127) < 1 and abs(fast - 440) < 2, (slow, fast)
    p.dry_wet = 0
    p.parameters["tape_pressure"].raw_value = 1
    np.testing.assert_allclose(p(tone, rate), tone, atol=1e-6)
    print(f"Pressure 64 and release on a recorded 440 Hz tone: {slow:.1f} Hz, then {fast:.1f} Hz")


def pre_fold_curve(bundle):
    for mode in ("Off", "2x", "4x"):
        p = open_plugin(bundle)
        p.oversampling = mode
        p.dry_wet, p.feedback = 1., 0.
        for trim in (12., 24.):
            p.record_input_db = trim
            for knees in (.3, 1.6, 2.6, 5., -5.):
                level = .72 * abs(knees)
                knee, shoulder = .72, .72 * .65
                expected = level if level <= knee else knee + shoulder * np.tanh((level-knee)/shoulder)
                source = np.full((2,4096), .72 * knees * 10**(-trim/20), np.float32)
                result = p(source, 48000, buffer_size=127)[0,-256:].mean()
                # The oversampling FIR phases have slightly different DC gains at the knee.
                tolerance = 1e-6 if mode == "Off" else .0004
                assert abs(result-np.copysign(expected,knees)) < tolerance, (mode, trim, knees, result)
    print("PASS: pre-fold soft knee at high drive with native, 2x and 4x processing")


def run(bundle):
    tape_pressure_audio(bundle)
    pre_fold_curve(bundle)
    for sr in (44100, 48000, 96000):
        for speed in (1, 0.5, 0.58):
            for synced in (False, True):
                impulse_test(bundle, sr, speed, synced)
    print("PASS: stereo head placement, continuous and snapped head and transport timing at three sample rates")
    direct_gain_and_cross_feedback(bundle)
    feedback_treble(bundle)
    native, twice, four = [record_aliasing(bundle, mode) for mode in ("Off", "2x", "4x")]
    assert twice < -50 and native > twice + 10 and four < twice - 10, (native, twice, four)
    feedback_controls(bundle)
    p = open_plugin(bundle)
    assert all(getattr(p, f"tap_{head}_active") for head in range(1, 5))
    rng = np.random.default_rng(42)
    x = rng.uniform(-0.25, 0.25, (2, 48000)).astype(np.float32)
    p.dry_wet = 0
    np.testing.assert_allclose(p(x, 48000), x, atol=1e-6)
    p.dry_wet = 1
    for head in range(1, 5):
        setattr(p, f"tap_{head}_active", False)
    np.testing.assert_array_equal(p(x, 48000), np.zeros_like(x))
    for head, pos in enumerate((-4, 5, -6, 7), 1):
        place(p, head, pos)
    p.feedback = 1
    p.record_input_db = 24
    y = p(x, 48000, buffer_size=31)
    # At maximum the first print and a full-level saturated return can add in phase.
    peak = abs(y).max()
    assert np.isfinite(y).all() and peak < 3, peak
    print(f"Four heads at +24 dB record drive and full feedback: {20*np.log10(peak):+.1f} dBFS peak")
    p.bypass = True
    np.testing.assert_allclose(p(x, 48000), x, atol=1e-6)
    p.bypass = False
    p.reel_speed = 0
    np.testing.assert_array_equal(p(x, 48000), np.zeros_like(x))
    print("PASS: dry null, four muted taps, heavily driven four-head feedback, bypass and stopped tape")
    fast, slow = [tone(bundle, s, 10000, 1)[0] for s in (1, 0.5)]
    assert 5 < fast - slow < 8, (fast, slow)
    quiet = [tone(bundle, s, 10000, 0.005)[0] for s in (1, 0.5)]
    assert abs(quiet[0] - quiet[1]) < 1
    for hz in (333, 10000):
        for speed in (1, .5):
            level, third = tone(bundle, speed, hz, 10**(-18/20))
            assert abs(level + 18) < .05 and third < .01, (hz, speed, level, third)
    level, third = tone(bundle, 1, 333, 10**(-6/20))
    assert abs(level + 6) < .05 and third < .01
    hot_thirds = [tone(bundle, s, 333, 1)[1] for s in (1, .5)]
    assert .5 < hot_thirds[0] < hot_thirds[1] < 5.
    print(f"10 kHz driven fundamental: {fast:.2f} / {slow:.2f} dBFS at full / half speed")
    print(f"10 kHz quiet fundamental: {quiet[0]:.2f} / {quiet[1]:.2f} dBFS")
    print("333 Hz H3 at 0 dBFS, full and half speed:", ", ".join(f"{h:.2f}%" for h in hot_thirds))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    run(parser.parse_args().bundle)
