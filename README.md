# Tappy Too Stereo 4Tap Delay

OtherNightStudio · Windows x64 VST3 · **1.0.1 Beta**

A four-tap stereo tape delay with tempo-synced heads, a variable-speed reel, pressure braking, and adjustable stereo cross-feedback.

Download the Windows x64 ZIP and extract it before installing. The same release includes the complete Visual Studio 2022 source package. [Download the beta](https://github.com/OtherNightStudio/TappyToo_Delay_VST3_Beta_V1.0.1/releases/tag/v1.0.1-beta).

## License

OtherNightStudio's original Tappy Too code and artwork are MIT-licensed. The distributed plug-in links JUCE 8.0.17 and is licensed as a whole under AGPLv3. See the [MIT license](LICENSE), [AGPLv3 license](COPYING), and [dependency notices](THIRD_PARTY_NOTICES.md).

## Play

Copy the complete `TappyToo.VST3` bundle to your DAW’s VST3 search folder, then rescan. The standard shared Windows folder is `C:\Program Files\Common Files\VST3`. Insert the effect on a stereo track. Load the supplied `Stereo Eighths.vstpreset` through the host’s VST3 preset browser for an audible starting point.

All four taps start active (olive). Click a numbered switch to mute that head (salmon). Drag or click its lane to place it. The center is a zero-time stereo record monitor. The left and right sides send the echo toward that speaker, reaching full placement at the first printed division. The first interval on either side blends continuously from stereo center.

With Sync off, the LED stays dark and heads move freely. Click Sync to place the heads on the nearest engraved fractions and the speed lever on its nearest sixteenth of full travel (Stop plus 16 running positions). Switching Sync off leaves them there. Drag any one of them to move it freely again. The light pulses once per host quarter note. Timing uses host BPM in both modes and starts at 120 BPM if the host supplies no tempo. One full-length division represents four quarter notes at full speed.

Drag the speed lever or click its scale. The V3 print shows all 17 detents, with fractions labeled at each eighth and a short tick at each intervening sixteenth. Speed changes the tape transport, echo times, pitch during transitions, and record headroom. At 120 BPM and full speed, the reel turns at 40 RPM and the tape model runs at its 7½ ips reference. Half speed doubles the delay times. Zero speed stops the tape and fades the wet signal. Input controls record drive. Feedback sends active delayed heads through the record stage again. The center monitor is excluded from feedback. Drag the feedback marker vertically. The loop reaches unity at 90% and drives progressively harder into tape saturation above it, reaching 1.6 at full feedback. Pull below 90% to let an oscillating loop decay. The decay time depends on the active tap distances and speed. Dry/Wet blends the echoes with the latency-aligned dry signal.

Record drive uses the pre-fold, speed-dependent soft knee on both the input and the feedback return. Low levels pass linearly, and hot levels approach a smooth plateau. Input is unity at noon.

With the DAW transport stopped, click the reel face to choose Load preset or Save preset as and browse any folder for a Tappy Too .vstpreset. A new instance starts with a bare reel. A successful save or load adds the label. The speed lever stays available for speed gestures. The torn masking tape carries the filename in handwriting and rotates with the reel during playback. The name is recalled with the DAW session.

The left knob below Feedback sets the return cutoff from 20 Hz to 20 kHz at reference speed. The first echo keeps its own tape bandwidth. The readout switches to kHz at 1,000 Hz. Click x2 or x4 under Oversampling to select a record mode, or click the selected label again for native rate. The knob with crossed arrows sets the share of each return sent to the opposite channel. At 0% repeats stay on their side, at 50% each side receives an equal share, and at 100% a single hard-panned tap alternates on successive repeats. The Feedback fader sets total regeneration. New instances start with 0% cross-feedback. Presets from the earlier fixed-crossfeed builds recall 85%. The salmon dot marks the oversampling selection. The defaults are 2× oversampling and a 12.5 kHz feedback cutoff.

## Tape pressure

While the DAW is playing, press and hold the reel to apply Tape Pressure. A mouse press starts at 64. Drag down to increase pressure or up to ease off. Release to send zero and let the motor recover to the selected lever speed. The reel animation follows the actual motor, including its acceleration and braking.

Automate **Tape Pressure** from 0 to 127. Zero leaves the motor free, 64 runs it at about half its selected speed, and 127 brings it to a stop. Pressure acts continuously between the lever detents, including with Sync on. The motor changes the speed and pitch of tape already recorded. The dry signal stays available through the mix control. Presets recall released pressure.

Wheel and arrow keys adjust the focused controls. Space or Enter operates a focused switch. Shift-drag makes fine relative knob adjustments. Double-click a continuous control to restore its default.

## Edit and build

Open `Builds/VisualStudio2022/TappyToo.sln` in Visual Studio 2022 with the Desktop development with C++ workload, v143 toolset, and a Windows SDK. Select **Release | x64** and build the solution. Edit the C++ and header files under **TappyToo – Shared Code → Source**. The VST3 target packages the effect, and the JUCE manifest helper writes its metadata.

The build produces `Builds/VisualStudio2022/x64/Release/VST3/TappyToo.vst3`. Debug and Release both target VST3. All framework paths are relative. The source distribution includes the required JUCE 8.0.17 modules and their license.

- `Source/TapeEngine.*` contains the stereo circular tape, four interpolated read heads, variable transport, selectable oversampling for record saturation, equalisation, and stereo cross-feedback. At the reference tape speed, the first pass has a 21 kHz low-pass target and the feedback return has a 12.5 kHz default target. Both follow tape speed and sample-rate limits. The host-reported latency is 16 samples.
- `Source/PluginProcessor.*` contains host timing, parameter automation, bypass, latency reporting, and session state.
- `Source/PluginEditor.*` draws and operates the live panel. The static panel print is cached at display resolution. The three-hole reel rotates independently of the yellow speed pointer.
- `TappyToo_CrossFeed.svg` is the original panel artwork. `Assets/CrossFeed_Print.svg` is its static print with the movable indicators removed. After changing the embedded SVG, re-save `TappyToo.jucer` with Projucer to refresh BinaryData before rebuilding. Normal C++ edits build directly in Visual Studio.

## Verification

The Release bundle passed Steinberg VST3 SDK 3.8.0 validation. `Tests/vst3_audio.py` exercises the compiled Windows module with NumPy and Pedalboard. It checks timing and stereo placement at 44.1, 48, and 96 kHz, bypass, muted heads, motor stop, driven feedback, and level-dependent saturation. `Tests/EditorCheck.cpp` exercises the actual editor controls, host beat/seek handling, and state recall against the shared-code target.

The record model is voiced from the Braun TG 1000 and RTM LPR35 specifications. The very slow settings extend the voicing beyond the published LPR35 measurements. With the center record monitor, unity input, and a 0 dBFS 10 kHz sine, the measured fundamental is −0.44 dBFS at full speed and −5.82 dBFS at half speed in a 48 kHz session. A quiet −46.02 dBFS tone at the same frequency measures −46.02 dBFS at both speeds. In the single-head feedback test at full speed, 10 kHz relative to 1 kHz falls by about 3 dB between the first and fourth echoes.
