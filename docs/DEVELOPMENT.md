# Building Kick

How to build the Kick application from this repository on macOS, a Linux desktop and Linux arm64,
and what had to change to make the public release build at all. Section 9 lists the hooks for
testing the app without a unit, all of them inert on the device.

## 1. What gets built

| Piece | Where | Role |
|---|---|---|
| `Kick/` | JUCE C++ application | The product: touchscreen UI, sequencer, sampler, audio engine |
| `AudIO/` | JUCE modules | Audio graph / DAW engine library used by Kick |
| `ComProtocol/PktMgr` | small C++ class | Framed packet protocol between the app and the control-surface microcontroller |
| `Linux/` | Buildroot tree | The i.MX8MM Linux image that runs the app on the device |

The CMake target is `Kick` (`Kick/apps/kick`). It is the same target everywhere:

| Platform | Output | Notes |
|---|---|---|
| macOS | `Kick/build/Kick.app` | |
| Linux desktop (x86_64) | `Kick/build/apps/kick/Kick_artefacts/Release/Kick` | |
| Linux arm64 | `Kick/build/apps/kick/Kick_artefacts/Release/Kick` | The device binary. On the unit it is installed as `/usr/bin/KT_Kick` and started by `ktkick.service` through `/usr/bin/kt_kick.sh`. `buildApp.sh` builds it with the Buildroot SDK and copies it out as `KT_Kick` |

The other targets in `Kick/apps/` (`KickMain`, `KickUserInterface`, `KickAudioEngine`,
`KickAudioModel`, `KickDeviceControl`, a split multi-process variant) are configured too but are
not needed.

## 2. Prerequisites

- CMake 3.16 or newer, git, a C++17 compiler.
- **macOS:** Xcode or the Command Line Tools (verified with Xcode 26.5 on macOS 26, Apple Silicon).
- **Linux (Debian / Ubuntu):** the package list from `Kick/how-to/Kick_Compilation.txt`:

  ```sh
  sudo apt install gcc g++ make cmake git pkg-config ninja-build \
      libasound2-dev libjack-jackd2-dev libcurl4-openssl-dev libfreetype6-dev \
      libx11-dev libxcomposite-dev libxcursor-dev libxext-dev libxinerama-dev \
      libxrandr-dev libxrender-dev libglu1-mesa-dev mesa-common-dev libserial-dev \
      libwebkit2gtk-4.0-dev     # Ubuntu 24.04 and Debian 13: libwebkit2gtk-4.1-dev
  ```

  The list in the how-to also names generic tools (`patch`, `rsync`, `cpio`, `bc`, ...) that
  Buildroot needs; the app build does not. WebKit is not linked (`JUCE_WEB_BROWSER=0`), so either
  WebKit version works.
- **Network on the first configure:** CPM downloads JUCE 7.0.12 (about 150 MB) into
  `.3rdPartySourceCache/` at the repository root (git-ignored) and applies
  `Kick/patches/juce_multitouch_7.0.12.patch` to it once. Later configures reuse it. Use
  `-DCPM_SOURCE_CACHE=<dir>` to share one cache between checkouts.

AudIO and ComProtocol are taken from this repository; no access to the internal git server named
in `Kick/3rdParty.cmake` is needed (section 4.1).

## 3. Build

From the repository root.

### macOS

```sh
MACOSX_DEPLOYMENT_TARGET=12.0 cmake -S Kick -B Kick/build -DCMAKE_BUILD_TYPE=Release -DTIMESTAMP=OFF
cmake --build Kick/build --target Kick -j8
```

`MACOSX_DEPLOYMENT_TARGET=12.0` must be in the environment of the configure. JUCE builds a helper,
`juceaide`, in a nested CMake run that does not inherit the project's deployment target; against
the macOS 15+ SDK it then fails on the obsoleted `CGWindowListCreateImage`.

### Linux desktop

```sh
cmake -S Kick -B Kick/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTIMESTAMP=OFF
cmake --build Kick/build --target Kick
```

### Linux arm64 (the device binary, `KT_Kick`)

On an arm64 Linux machine the Linux desktop commands above build it natively. The binary is
`Kick/build/apps/kick/Kick_artefacts/Release/Kick`; rename it to `KT_Kick`.

On an Apple Silicon Mac, Docker runs arm64 Linux containers natively:

```sh
docker run --rm -it --platform linux/arm64 -v "$PWD:/src" -w /src debian:bookworm bash
# inside: apt-get update && apt-get install -y <the package list above>, then the Linux commands
```

A native build links against the distribution's libraries (glibc, ALSA, FreeType, X11). For the
unit itself, build with the Buildroot SDK so it links against the image's libraries: build the
SDK from `Linux/` (`make KTKick_defconfig && make sdk`), then
`TOOLCHAIN_FILE=<sdk>/share/buildroot/toolchainfile.cmake sh buildApp.sh Release` in `Kick/`
(see `Kick/how-to/Kick_Compilation.txt`).

Options: `-DTIMESTAMP=OFF` skips the "fresh timestamp every build" helper (harmless either way).

## 4. Run

The app resolves its data directories at run time (`Kick/modules/common/source/kick_paths.cpp`):

| | macOS (relative to the working directory) | Linux (absolute, as on the device) |
|---|---|---|
| assets | `./assets` | `/app/assets` |
| demos, templates | `./demos`, `./templates` | `/app/demos`, `/app/templates` |
| samples | `./data/samplesKick` | `/data/sounds` |
| projects, logs, prefs | `./projects`, `./logs`, `./prefs` | `/home/Kick/projects`, `/home/Kick/logs`, `/home/Kick/prefs` |

The configure copies `assets/`, `templates/` and `demos/` into the build directory.

- **macOS:** start it from the build directory, `cd Kick/build && ./Kick.app/Contents/MacOS/Kick`.
  `open Kick.app` starts it with `/` as the working directory and nothing loads. The first launch
  asks for microphone access (the engine opens a stereo input); denying it is fine.
- **Linux:** put the data where the device has it, for example
  `sudo mkdir -p /app && sudo ln -s "$PWD/Kick/build/"{assets,templates,demos} /app/`, then run the
  binary.

Logs go to stdout. A good start prints `AudioDevice: using '<device>' at 48000 Hz, ...`.
Loading a project prints `Project: loaded <path>`.

Screens that drive the unit's hardware (Wi-Fi, Ethernet, CV, firmware update, the resource
meters) say "not available on this system" on a computer, and the app does not touch the
computer's network or clock. For scripted checks, the audio probe and the other test hooks, see
section 9.

Without the control-surface microcontroller there are no pads, buttons or knobs. On macOS (and
Windows) the keyboard drives them instead (`Kick/modules/contracts/utils/keyboard`). On Linux the
serial port is a stub (4.2), so not even a unit's control surface is reachable yet.

## 5. What had to change to make the public release build

The release notes said the build "will not work out of the box". These are the reasons, and what
was done about each. None of it changes what the device code does.

### 5.1 AudIO was packaged differently than Kick expects

Kick includes two JUCE modules, `AudIO_juce` and `AudIO_constants`, and expects a
`juce_modules.cmake` in the AudIO root. The public AudIO ships one module (`AudIO/AudIO/AudIO.h`)
and no cmake file. Added:

- `AudIO/juce_modules.cmake`: registers the three modules.
- `AudIO/AudIO_constants/AudIO_constants.h`: **reconstructs** the constants that only lived in the
  internal module. The values are inferred from how Kick and AudIO use them, not copied; the
  reasoning is the comment beside each value:

  | Constant | Value | Reasoning |
  |---|---|---|
  | `MIDI::MIN/MAX/STEP` | 0 / 127 / 1 | 7-bit MIDI |
  | `MIDI::NUMERIC_MAX` | 128 | used as an exclusive bound in loops |
  | `MIDI::SCALER` | 1/127 | source comment "(MidiLevel / 127)" |
  | `MIDI::LEVEL_DEFAULT` | 100 | default pad / track level, also an upper knob bound |
  | `MIDI::DEFAULT` | 0 | default modulation amount; also a slider interval, where 0 means continuous |
  | `MIDI::MIDDLE_C`, `MIDI::NOTE_DEFAULT` | 60 | |
  | `MIDI_CH::MIN/MAX/DEFAULT` | 1 / 16 / 1 | shown 1-based in the UI |
  | `POLYPHONY::MIN/MAX/DEFAULT` | 1 / 64 / 16 | `ISynthObject` allocates `MAX` voices up front |
  | `GAIN::MIN/MAX/STEP/DEFAULT` | 0 / 1 / 0.01 / 1 | linear gain into `AudIO::Panner`; knobs use a 0.01 step |
  | `GAIN::ROOT_2` | 0.7071 (-3 dB) | default metronome gain |
  | `PAN::MIN/MAX/STEP/DEFAULT` | -1 / 1 / 0.01 / 0 | `AudIO::Panner` documents -1 (left) .. 1 (right) |
  | `DB::MIN/MAX` | -100 / 0 | `AudIO::utils::linearTolog` returns -100 for silence |
  | `UNKNOWN_VALUE` | -1 | sentinel for an unattached `juce::Value` |
  | `AUDIO_SAMPLE_RATE` | 48000 | |
  | `AUDIO_BLOCK_SIZE` | 480 | must equal AudIO's memory-pool block (`BLOCK_SIZE` in `Daw/DawEngine.cpp`) |
  | `AUDIO_NUM_INPUTS/OUTPUTS` | 2 / 2 | |
  | `AUDIO_NUM_SEND_RETURNS` | 4 | sends 1-4 are referenced |
  | `AUDIO_NUM_SUBMIX` | 4 | the MIDI control UI lists "Submix 1..4" |
  | `AUDIO_NUM_AUX` | 2 | a guess, nothing in the code pins it |

  If any of these turn out wrong, this header is the only place to fix them.
- `AudIO/AudIO_juce/`: the JUCE glue, **reconstructed from how Kick calls it**, in namespace `mt`:
  - `mt::AudioDevice`: `AudIO::IAudioDevice` on top of `juce::AudioDeviceManager`. Re-blocks
    through FIFOs if the host device's callback size differs from AudIO's 480-sample block.
  - `mt::ADSR`: linear attack / hold / decay / sustain / release envelope, lengths in samples.
  - `mt::Filter` and `FilterType`: stereo state-variable filter (low / high / band pass).
  - `mt::Recorder`, `mt::SampleRecorder`, `mt::Looper`: an AudIO object that records its input
    bus into a `juce::AudioBuffer`, with threshold-armed start, overdub, loop, reverse, preview,
    WAV save, and (Looper) an insert chain and single-level undo. The recorder sets its routing
    id from its object id, as AudIO's own recorder does, or it is never patched.

  The behaviour is plausible, not bit-exact with the original.
- `AudIO/AudIO/Includes/Platform.h`: includes `<thread>` and `<chrono>`, which the DAW sources use
  but never included.

### 5.2 `juce_serialport` was removed

`Kick/modules/3rdParty/juce_serialport` had no licence and was deleted from the release, but
`modules/3rdParty/CMakeLists.txt` still lists it and `modules/input_events` uses it to talk to the
control-surface microcontroller on `/dev/ttymxc0`. Added an API-compatible **stub** at the same
path: the port never opens, reads are empty, writes are discarded. That is enough on a desktop,
where there is no `/dev/ttymxc0`. **A device build needs a real implementation**: every Linux
build prints a compiler message saying so (a `#pragma message`: the project compiles with `-w`,
which hides `#warning`).

### 5.3 Small Kick fixes

- `Kick/3rdParty.cmake`: when `MT_PLATFORM_GIT_PATH` is the placeholder and neither
  `CPM_AudIO_SOURCE` nor `CPM_COM_PROTOCOL_SOURCE` is given, use the in-tree `AudIO/` and
  `ComProtocol/`. The configure prints `AudIO: using the in-tree copy ...`. To use other copies,
  pass `-DCPM_AudIO_SOURCE=<dir>` / `-DCPM_COM_PROTOCOL_SOURCE=<dir>`, or set
  `-DMT_PLATFORM_GIT_PATH=<git base url>` to fetch them by tag as before.
- `Kick/modules/common/source/kick_Constants.h`: `EMPTY::String`;
  `Kick/modules/engine/interface/interface.h`: `EMPTY::SampleBuffer`. Both were used but defined
  nowhere.
- `Kick/modules/hardware/source/kick_WifiFirmware.cpp`: `#include <sys/stat.h>` for `stat()`.
- `Kick/modules/contracts/utils/keyboard/keyboard_detector.{h,cpp}`: the desktop keyboard map is
  enabled on macOS as well as Windows.
- `Kick/apps/kick/CMakeLists.txt`: `MICROPHONE_PERMISSION_ENABLED TRUE`, otherwise macOS kills the
  app when the engine opens its input.
- Root `.gitignore`: the CPM cache.

### 5.4 Still missing from the release

- The Myriad Pro font that `assets/Default/theme.xml` names. Without it JUCE's default sans-serif
  is used. Drop a `.ttf` you have the rights to into `Kick/assets/Default/` and name it there.
- Factory samples, metronome sounds and the General MIDI SoundFont. The demo and template
  projects are empty `<ROOT>` documents.
- The prebuilt aarch64 SDK (section 3).
- A real `juce_serialport` (5.2).

## 6. Continuous integration

`.github/workflows/build.yml` runs on every pull request and on pushes to `main`:

| Job | Runner | Builds |
|---|---|---|
| `Kick (macOS)` | `macos-14` | `Kick.app` |
| `Kick (Linux x86_64)` | `ubuntu-24.04` | `Kick` |
| `KT_Kick (Linux arm64)` | `ubuntu-24.04-arm` | `Kick`, packaged as the `KT_Kick-linux-arm64` artifact |

The `KT_Kick-linux-arm64` artifact holds `KT_Kick-linux-arm64.tar.gz` (the binary as
`KT_Kick` plus the `assets/`, `templates/` and `demos/` it reads from `/app` at run time, and
`manifest.json`) and `manifest.json` next to it: git ref and commit, build date, compiler, runner
image, JUCE version, and the shared libraries the binary needs. It is a native Ubuntu 24.04 arm64
build, not a Buildroot SDK build. The CPM download cache is kept between runs, keyed on the CPM
and JUCE versions and the JUCE patch.

## 7. Troubleshooting

- **"Failed to build juceaide ... CGWindowListCreateImage is unavailable"** (macOS): the configure
  ran without `MACOSX_DEPLOYMENT_TARGET=12.0`. Delete `Kick/build` and configure again with it.
- **"include could not find requested file: .../AudIO/juce_modules.cmake"**: `CPM_AudIO_SOURCE`
  or `MT_PLATFORM_GIT_PATH` points somewhere else. Leave both unset to use the in-tree copy, or
  pass the absolute path to this repository's `AudIO/`.
- **CPM tries to clone from `https://example.com/your-git-server`**: an old build directory has
  the source variables cached empty. Delete the build directory and configure again.
- **Black screen or missing graphics**: on macOS the app was not started from `Kick/build`; on
  Linux `/app/assets` does not exist.
- **No sound**: look for `AudioDevice: using '...'`. "no audio device available" means JUCE could
  not open a default device; check the system's output device (or ALSA / JACK on Linux).
- **JUCE is downloaded again on every configure**: keep the default cache
  (`.3rdPartySourceCache/`) or pass the same `-DCPM_SOURCE_CACHE=<dir>` every time.
- **"#pragma message: juce_serialport is a stub" on Linux**: expected, see 5.2.

<!-- Section 8, the control-surface link and its tests, comes with feature/serialport (Kick/tests refers to it). -->

## 9. Extension points for testing without a unit

A few small hooks let a desktop build of Kick, or a test harness such as an emulator, run and
check the app without the unit's hardware. **On a unit they do nothing**: each one is either off
unless an environment variable is set, or it only acts when a piece of hardware is missing, and a
unit has all of it. None of them contains emulator code.

### 9.1 Environment variables

All of them are unset on a unit (`ktkick.service` sets none of them).

| Variable | What it does | Where | On the unit (unset) |
|---|---|---|---|
| `KICK_SERIAL_DEVICE=<absolute path>` | Opens that device for the control-surface link instead of `/dev/ttymxc0`, with the same settings (230400 8N1) and handshake: a USB serial adapter, or the pseudo-terminal of an emulated panel | `modules/input_events` (from `feature/serialport`) | `/dev/ttymxc0`, as always |
| `KICK_TEST_CONTROL=<file>` | Starts the test-control module (9.3): the app polls `<file>` for commands that touch the screen, list or render it, and read the transport | `modules/test_control`, created by `apps/kick/Main.cpp` | Not created: nothing is polled or logged, focus is JUCE's alone |
| `KICK_AUDIO_PROBE=<file.wav>` | `mt::AudioDevice` writes the engine's master output to that WAV (32-bit float) and logs `AudioProbe: t=<n>s peak=<x>dBFS rms=<y>dBFS` once per second of audio (`-120.0dBFS` is silence). The audio thread only copies into a lock-free FIFO; a background thread writes | `AudIO/AudIO_juce/source/mt_AudioDeviceProbe.*` | No probe object; the audio callback is unchanged |
| `KICK_AUDIO_INPUT=silence\|device\|tone[:hz]\|pulse[:ms]\|<file>` | Replaces the device input on every channel with digital silence, a -12 dBFS sine (1 kHz default), 50 ms 1 kHz bursts at -6 dBFS every 500 ms (or `<ms>`), or a looped audio file; `device` (or unset) keeps the real input. Logged as `TestInput: ...` | same | The real input |
| `KICK_LOG_NOTES=1` | The sequencer logs every note it plays: `Note: track '<name>' tick <t> pos <p> note <n> velocity <v> id <id>`. Read once | `engine/audio_engine/engine/mt_audio_track.cpp` | Nothing logged |
| `KICK_HARDWARE=none\|<list>` | Overrides the hardware probe (9.2): `none` makes every feature unavailable, `wifi,clock` makes exactly those available, on any platform. Read once and logged once (`Hardware: KICK_HARDWARE=...`) | `modules/hardware/source/kick_HardwareProbe.*` | The probe decides, and finds everything |

Paths are relative to the working directory. One log line is always written, on every platform:
`Project: loaded <path>` when a project is loaded (`MTAudioManager::loadProject`).

### 9.2 The hardware probe

Some screens and helpers exist only to drive the unit's hardware or its Linux: they run `ifconfig`,
`killall -9 wpa_supplicant`, `timedatectl`, open `/dev/i2c-3`, or scan for `.swu` images.
`mt::hw::isAvailable( Feature )` says whether this system has the feature:

| Feature | Available when (Linux; never on other platforms) | Consulted by | When unavailable |
|---|---|---|---|
| `wifi` | `/sys/class/net/wlan0` exists | `WifiManager::executeCommand`, `scanWiFiNetworks`; PREFS > WiFi | commands and scan are not run (logged); the tab is covered |
| `ethernet` | `/sys/class/net/eth0` exists | `HwPref::startDhcp`, `stopDhcp`, `ethernetEnable`, `setIP`; PREFS > Ethernet | commands are not run (logged); the tab is covered |
| `cv` | `/dev/i2c-3` exists | PREFS > CV (and the calibration screen behind it) | the tab is covered |
| `update` | `swupdate` is on `PATH` | Firmware Update (`SWUpdateView`) | no image scan, no USB gadget; the list is covered |
| `clock` | always | `HwPref::setDate`, `setTime`, `setTimeZone` | the clock is not set (logged) |
| `resources` | always | the CPU / RAM / storage meters (`KSystemResources`) | no 1 Hz refresh; the meters are covered |

It is checked on every call, so an interface that comes up after the app (`wlan0` once its
firmware has loaded) is picked up the next time the screen is shown. A command that is skipped
logs `FeatureUnavailable: <feature>: command not run: <command>`.

A covered screen shows `ui::FeatureUnavailable` (`kick_ui_controls/source/kick_FeatureUnavailable.h`):
`<feature>: not available on this system` with a line on what the unit does there. It swallows
touches, so the controls underneath cannot be used, and logs `FeatureUnavailable: <feature> shown`
/ `hidden`. It is created only while the feature is unavailable, so on a unit it never exists.

On macOS every feature is unavailable. On a Linux desktop the probe usually finds `eth0` (and
maybe `wlan0`): set `KICK_HARDWARE=none` so the app never reconfigures your computer's network or
clock. In a VM that should behave like a unit, leave it unset.

### 9.3 The test-control module

`modules/test_control` is a generic, file-driven test channel for the screen, for scripted
checks. With `KICK_TEST_CONTROL=<file>` set the app logs `TestControl: listening on <file>`, then
reads lines appended to `<file>` after start-up (it creates the file if needed) and runs one at a
time on the message thread:

```
<seq> <command> [args]          e.g.  7 click "Firmware Update"
```

When a command is done the app logs `TestControl: ok <seq>`, or `TestControl: error <seq>
<reason>`. A client appends a line and waits for its acknowledgement; a blocked message thread
shows up as a timeout.

| Command | What it does |
|---|---|
| `tap X Y` | Touches pixel (X, Y) of the 1024x600 screen (the main window) |
| `click NAME` | Touches the centre of the showing component whose title (or name) is NAME, popups first: Preferences tabs `Info`, `Hardware`, `CV`, `Ethernet`, `WiFi`, buttons such as `Settings`, `Resources`, `Firmware Update` |
| `clickat NAME FX FY` | Touches NAME at fraction (FX, FY) of its size, for controls with untitled parts |
| `list [FILTER]` | Logs `TestControl: item <type> <name> <x y w h> screen\|popup` for every showing, titled component (bounds in screen pixels, usable with `tap`) |
| `shot OUT.png` | Renders the main window, popups and bubbles included, to `OUT.png` at 1024x600, in-process (no window server needed); any other desktop window to `OUT-<n>.png` |
| `windows` | Logs the main window, the overlays on it (modal popups, bubbles) and any other desktop window, each with its bounds on the screen and on the display |
| `status` | Logs `TestControl: status <state> bbt=<bar>:<beat>:<tick> bpm=<n> clock=<tempo> rec=n/a metronome=on\|off` (BAR:BEAT:TICK 1-based as on screen; the record mode needs the note-recording engine and prints `n/a` until then) |
| `echo TEXT` | Logs `TestControl: echo TEXT`, a marker for scripts |

How it works, and what it does not do:

- **Touches** are delivered to the component under the point directly: buttons get
  `triggerClick()`, anything else `mouseDown` and, 80 ms later, `mouseUp`. They do not go through
  the window's `ComponentPeer`, whose hit-test asks macOS whether the window is the topmost one
  there, which is false while the screen is locked or the window is covered. A touch on a
  component behind a modal dialog is refused (`blocked by modal component ...`). Mouse listeners
  on parent components do not hear these touches.
- **Focus.** A touched control gets the jog wheel, + / -, PUSH and the slider through JUCE
  keyboard focus, which JUCE grants only while the window is the key window. The module records
  the control a touch focused (`testcontrol::FocusFallback`), and `ui::getComponentInFocus()` uses
  it while JUCE has no focused component. The fallback is active only while the module runs.
- **Panel input** (keys, pads, encoders, slider) is not part of it. It comes over the serial
  port, from the unit's control surface or from an emulator playing its part.

### 9.4 Popups and lists on a desktop

Two generic changes make the screen behave on a desktop as on the unit, with no switch:

- Full-screen popups (dialogs, the loading spinner) and notification bubbles are added to the
  app's main window (`ui::overlayRoot()`, `kick_ui_controls/source/kick_ControlBase.h`, set by
  `apps/kick/Main.cpp`), not placed on the desktop at display coordinates. On the unit the window
  is the display, so they appear where they always did; on a desktop they stay inside the Kick
  window instead of covering the top-left of the computer's display. The blurred backdrop is a
  snapshot of that window.
- List rows (`KickTableView`) are hit-tested in the list's own coordinates, and the Q-Link row
  maths measures the list from the top-level component rather than the desktop. On the unit both
  give the same numbers.
