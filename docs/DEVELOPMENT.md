# Building Kick

How to build the Kick application from this repository on macOS, a Linux desktop and Linux arm64,
and what had to change to make the public release build at all.

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
