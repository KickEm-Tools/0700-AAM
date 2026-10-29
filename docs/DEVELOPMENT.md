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
Windows) the keyboard drives them instead (`Kick/modules/contracts/utils/keyboard`). On Linux and
macOS the app talks to a control surface over a serial port: `/dev/ttymxc0` on the unit, or the
device named by `KICK_SERIAL_DEVICE` (section 8), for example the pseudo-terminal of
`Kick/tests/fake_mcu.py`.

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
control-surface microcontroller on `/dev/ttymxc0`. It has been **written again from scratch** at
the same path, on termios, with the API `input_events` uses (section 8.6). It works on Linux and
macOS. Windows has no termios, so there the port never opens (a `#pragma message` says so).

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
- The control-surface microcontroller's firmware. Its side of the protocol is described in
  section 8 from what the app expects.

## 6. Continuous integration

`.github/workflows/build.yml` runs on every pull request and on pushes to `main`:

| Job | Runner | Builds |
|---|---|---|
| `Kick (macOS)` | `macos-14` | `Kick.app` |
| `Kick (Linux x86_64)` | `ubuntu-24.04` | `Kick` |
| `KT_Kick (Linux arm64)` | `ubuntu-24.04-arm` | `Kick`, packaged as the `KT_Kick-linux-arm64` artifact |

Every job also builds and runs the control-surface link tests (8.7) with `ctest`.

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
- **No control surface**: look for `InputEvents: control surface on ...`. No such line means the
  device file does not exist (on the unit, `/dev/ttymxc0`; otherwise check `KICK_SERIAL_DEVICE`,
  which must be an absolute path). `could not open` comes after a `SerialPort open:` line with
  the reason (permissions: the user needs the device's group, often `dialout`). If the line says
  `open` but nothing reacts, see 8.3: the surface has to answer the handshake.

## 8. The control-surface link

On the unit the keys, pads, knobs, slider, LEDs and pad colours belong to a microcontroller,
which talks to the app over a serial port. Its firmware is not in this repository. Everything
below comes from the app's side:

- `ComProtocol/PktMgr` (framing);
- `Kick/modules/input_events/input_events.cpp` (`HwSerialData`: port, reader, send queue);
- `KickHome::handleCommand` in `Kick/modules/kick_ui/dialogs/kick_AppHome.cpp` (handshake);
- the ids in `Kick/modules/contracts/dcl/dcl_contracts.h` (`DeviceControlContracts`) and the
  message builders in `dcl_contracts_helper.cpp`.

It is checked by `kick_protocol_roundtrip` and the wire runs in 8.7.

### 8.1 The port

- **Device and settings.** `/dev/ttymxc0`, 230400 baud, 8 data bits, no parity, 1 stop bit, no
  flow control, raw.
- **When it opens.** `Controls::InputEvents::initialize()` opens it once at start-up (from
  `KickDeviceAdapter` in `apps/kick`). It opens only if the device file exists and
  `SerialPort::getSerialPortPaths()` lists at least one port.
- **`KICK_SERIAL_DEVICE=<absolute path>`** opens another device with the same settings and the
  same handshake: a USB serial adapter, or a pseudo-terminal. A path given this way does not have
  to be listed by `getSerialPortPaths()`. When the variable is unset, nothing changes.
- **Log line.** The app logs one line for the port:

  ```
  InputEvents: control surface on /dev/ttymxc0 (default), 230400 8N1, no flow control: open
  ```

  It says `(KICK_SERIAL_DEVICE)` instead when the variable chose the device, and `could not open`
  after a `SerialPort open: ...` line with the reason when opening failed. A missing
  `/dev/ttymxc0` logs nothing, as before.
- **Platforms.** The link is compiled on Linux and macOS. On Windows the port never opens.

### 8.2 Frames

Both directions use the same frame, built by `CPktMgr::BuildAndTransmitPacket`:

| Offset | Content | |
|---|---|---|
| 0, 1 | `'M'` `'T'` (`4D 54`) | start bytes |
| 2 | type | `CPktMgr::enumPacketType`: 0 SYSTEM, 1 BUTTON, 2 LED, 3 PAD, 4 KNOBS, 5 MIDI, 6 CV, 7 SLIDER |
| 3 | direction | 0 request, 1 response |
| 4 | command | per type: `SysCmd`, `BtnCmd`, `LedCmd`, `PadCmd`, `KnobCmd`, `SliderCmd` |
| 5 | size | payload length + 1 (it counts the CRC byte) |
| 6 .. 5+n | payload | n bytes |
| 6+n | CRC | see below |
| 7+n | trailing byte | **surface to app only**, see below |

- **Payload values.** `HWMsg_t::data` is a `std::vector<int>`. Only the low 8 bits of each value
  go on the wire, and they are read back as 0-255: -1 arrives as 255, and 300 as 44.
- **CRC.** CRC-8 with polynomial 0x31 (x^8 + x^5 + x^4 + 1), initial value 0xFF, MSB first, no
  reflection, no final XOR (the CRC-8/NRSC-5 parameters; the check value of `"123456789"` is
  0xF7). It covers every byte from `'M'` to the last payload byte.
- **The trailing byte.** The surface sends one byte after the CRC; the app's reader skips it
  (8.4). Before `fix/serial-robustness` the reader took `size + 7` bytes per frame and required
  it.
  `Kick/tests/fake_mcu.py` sends `'\n'`: the line `f_txData.push_back('\n')`, commented out at the
  end of `BuildAndTransmitPacket`, suggests this is what the microcontroller does. The app's own
  frames have no trailing byte.
- **Example.** `4D 54 00 00 04 02 01 A0 0A` is SYSTEM, request, `EN_SYS_CHECK_COMM_STATUS`,
  payload `[1]`, CRC 0xA0, trailing `'\n'`.
- **Size limit.** The app's receive FIFO is 20 bytes (`BUFFER_RX_SIZE`, `ComConfig.hpp`) and
  `PushByteToRxFifo` does not check bounds, so the reader drops a frame whose payload is larger
  (8.4). A surface-to-app payload must not exceed 20 bytes.
  Every packet the app handles has at most 4.

**Packets the app handles (surface to app):**

| Type | Command | Payload | What the app does |
|---|---|---|---|
| SYSTEM | `EN_SYS_CHECK_COMM_STATUS` (4) | `[1]` comm init, `[2]` comm live | handshake (8.3) |
| SYSTEM | `EN_SYS_GET_SW_VER` (1) | `[major, minor]` | stores the surface version |
| SYSTEM | `EN_SYS_SW_UPDATE` (3) | `[code]` | firmware-update screen |
| BUTTON | `EN_BTN_STATUS` (0) | `[key, state]` | key: `BtnLedSequence` (PLAY 45, SHIFT 31, ...); state: 0 released, 1 pressed, 2 long press, 3 double click |
| PAD | `EN_PAD_STATE` (0) | `[pad 0-15, state, velocity, aftertouch]` | |
| KNOBS | `EN_KNOB_ROTARY` (0) / `EN_KNOB_QLINK` (1) | `[knob, direction, steps]` | knob: 0 centre, 1-16; direction: 0 anticlockwise, 1 clockwise |
| SLIDER | `EN_SLIDER_TOUCH_INPUT` (0) | `[value]` | |

Responses (direction 1) from the surface are decoded and then ignored.

**Packets the app sends (app to surface):**

| Type | Command | Payload |
|---|---|---|
| SYSTEM | `EN_SYS_COMM_INIT` (2) | `[1]` when it connects, `[2]` in answer to comm init |
| SYSTEM | `EN_SYS_CHECK_COMM_STATUS` (4) | `[2]` in answer to comm live |
| SYSTEM | `EN_SYS_BPM` (5) | `[high, low]` of the tempo value |
| SYSTEM | `EN_SYS_SW_UPDATE` (3) | `[code]` |
| BUTTON | `EN_BTN_STATUS` (0) | `[key, 1]` (`setPlayStateMessage`) |
| LED | `EN_LED_SET_SINGLE_LED` (0) | `[key, state, colour]`; `[bank key, 1]` for the bank LEDs |
| LED | `EN_LED_BLINK_SINGLE` (1), `EN_LED_SET_ALL` (2) | defined; `isLEDBlinkMessage` / `isLEDAllMessage` exist |
| PAD | `EN_PAD_SET_SINGLE_PAD` (1) | `[pad, state, colour]` |
| PAD | `EN_PAD_SET_ALL_SINGLE_COLOUR` (2) | `[state, colour]` |
| PAD | `EN_PAD_SET_ALL_MULTI_COLOUR` (3) | `[state, 16 colours]`: 17 bytes |
| PAD | `EN_PAD_SETTINGS` (4) | as given, for example `[16, 4, 4, 4]` at start-up |
| PAD | `EN_PAD_COLOR_MODE` (5), `EN_PAD_SET_AFTER_TOUCH` (7) | `[value]` |
| PAD | `EN_PAD_SET_COLOUR_STATE` (6) | `[n colours, n states]`: 32 bytes for 16 pads |
| PAD | `EN_PAD_SET_BLINKING` (8) | `[pad, state, on colour, off colour]` |
| PAD | `EN_PAD_SET_STEP_SINGLE_PAD` (9) | `[pad, state, colour, velocity]` |
| PAD | `EN_PAD_SET_STEP_ALL_PAD` (10) | `[state, 16 colours, 16 velocities]`: 33 bytes, the largest frame (40 bytes) |
| SLIDER | `EN_SLIDER_BAR_LED` (1), `EN_SLIDER_LEVEL_LED` (2) | `[percent]` |

Notes on the tables:

- `EN_SYS_RECONNECT` and `EN_SYS_SHUTDOWN` are handled inside the app (8.5) and never sent.
- MIDI (5) and CV (6) are defined packet types, but nothing in Kick sends or handles them.
- **SHIFT (31).** The reader stores its state with `DclContractsHelper::setShiftPressed()`
  (pressed, long press and double click count as held), and `isShiftPressed()` reads it; the
  desktop keyboard map sets the same state. Before `fix/shift-state` the reader wrote a separate
  `Controls::shiftBtnState` that nothing read, so no SHIFT function triggered from a control
  surface.

### 8.3 Handshake

| # | Direction | Frame | App log |
|---|---|---|---|
| 1 | app to surface | SYSTEM `EN_SYS_COMM_INIT` `[1]` (`EN_SYS_COMM_CONNECTED`), as soon as the port is open | |
| 2 | surface to app | SYSTEM `EN_SYS_CHECK_COMM_STATUS` `[1]` ("comm init") | ` Data[0] : 1 Cmd : 4 type : 0` |
| 3 | app to surface | SYSTEM `EN_SYS_COMM_INIT` `[2]` | `Send Comm Init` |
| 4 | surface to app | SYSTEM `EN_SYS_CHECK_COMM_STATUS` `[2]` ("comm live") | ` Data[0] : 2 Cmd : 4 type : 0` |
| 5 | app to surface | SYSTEM `EN_SYS_CHECK_COMM_STATUS` `[2]` | `Send Comm Live` |

Timing and retries:

- **The app keeps no state and runs no timers.** It sends step 1 once per `connect()` and
  answers steps 2 and 4 each time they arrive.
- **Retries are the surface's job.** The answers come from `KickHome::handleCommand`, so a comm
  init or comm live that arrives before the home screen exists gets no answer. The surface must
  repeat steps 2 and 4 until answered. `fake_mcu.py` repeats them every second.
- **Measured.** No retry was needed in either run below; the times are from step 1 to the app's
  answer to comm live:

  | Run | Comm init answered | Comm live answered | LIVE |
  |---|---|---|---|
  | Plain app on macOS (8.7) | 0.44 s (the app was still starting) | 0.03 s | 0.47 s |
  | Linux arm64 build in a Debian container, through `/dev/ttymxc0` | 0.11 s | 0.003 s | 0.11 s |
- **Nothing waits for LIVE.** Keys, pads and knobs are handled whether or not the handshake
  happened. `KickHome` acts on keys only once a project is loaded.
- **Logging.** The app logs every request it receives, as ` Data[0] : <d0> Cmd : <cmd> type :
  <type>`.

### 8.4 What the reader does with imperfect input

The reader is `HwSerialData::changeListenerCallback`. It runs on the message thread, once per
change notification from the serial input stream, reads everything buffered and hands it to
`Controls::SurfaceFrameReader` (`modules/input_events/source/kick_SurfaceFrameReader.h`):

1. **Bytes are kept between notifications**, so a frame that arrives in two pieces is put back
   together.
2. **Anything that is not `'M'` `'T'` is skipped** up to the next `'M'` `'T'`. The one trailing
   byte after each frame is skipped silently; other skipped bytes are logged
   (`InputEvents: skipped 10 byte(s) before a frame: 00 FF ...`). The trailing byte is not
   required: a frame without it is taken as well.
3. **Each frame goes through `CPktMgr::PushByteToRxFifo` whole, from its `'M'`**, so CPktMgr's
   partial-frame buffer (a function-level `static`) is always emptied by its CRC state.
   - CRC matches: the frame is dispatched (requests only; responses are ignored).
   - CRC does not match: the frame is dropped (`InputEvents: dropped a frame with a bad CRC`),
     up to an `'M'` `'T'` inside it, where a good frame may start.
4. **Frames the app cannot take are dropped:** no payload (size 1; no handler expects an empty
   message, and `PushByteToRxFifo` never completes one), a size of 0, and a payload larger than
   the 20-byte receive FIFO, which used to overrun it.

Checked by `kick_protocol_roundtrip` (two frames in one read, frames with and without the
trailing byte, a bad CRC alone and before a good frame, a frame split across two reads, fewer
than 6 bytes, garbage before and between frames, an empty payload, an oversized payload) and,
over a pseudo-terminal against the whole app, by the emulator's `checks/serial_faults`
(KickEm-Tools/KickEmulator).

Until `fix/serial-robustness`, a read that did not start with a complete frame was dropped whole:
a frame split across two reads was lost with the frame after it, garbage before a frame lost it,
garbage between frames lost everything after it in that read, and an empty payload spoiled the
next frame's CRC.

### 8.5 Hang-ups and `reconnect()`

- **A hang-up** (a USB adapter unplugged, the other end of a pseudo-terminal closed): the serial
  port's reader logs `SerialPort read: <path>: end of file; reader stopped` and stops. From the
  first successful open on, a 1 s timer in `HwSerialData` checks that the reader is still
  running; once it is not, the app logs `InputEvents: the control surface port hung up; reopening
  it` and reopens the port once a second until it is back (`InputEvents: the control surface port
  is open again`). Reopening sends the handshake's first frame again, so the surface answers as at
  start-up. `/dev/ttymxc0` on the unit does not hang up, so there the timer only ever checks.
- **`reconnect()`** (`close()` then `connect()`) runs when the app is asked to send SYSTEM
  `EN_SYS_RECONNECT` (`DclContractsHelper::reconnectMessage()`; in this tree only
  `apps/kick_device_control_test` sends it). `close()` stops the send thread; `connect()` now starts
  it again (before `fix/serial-robustness` it did not, and every frame after a reconnect was queued
  and never sent).
- **Shutdown.** `EN_SYS_SHUTDOWN` calls `shutdown()`, which destroys the link.

### 8.6 `juce_serialport`

`Kick/modules/3rdParty/juce_serialport` has these parts:

- **`SerialPort`**
  - `open()` uses `O_RDWR | O_NOCTTY | O_NONBLOCK`, sets raw mode (`cfmakeraw`,
    `CLOCAL | CREAD`, `VMIN 0`, `VTIME 0`) and flushes what the driver had buffered.
  - `setConfig()` sets the baud rate, 5-8 data bits, parity (mark and space only where `CMSPAR`
    exists), 1 or 2 stop bits (termios has no 1.5) and flow control. `getConfig()` reads back
    what the device accepted.
  - A rate with a `B` constant is set with `cfset[io]speed`. 230400 is `B230400` on Linux and
    macOS.
  - On macOS any other rate is set with the `IOSSIOSPEED` ioctl, all or nothing. A pty refuses
    it. On Linux other rates are refused.
- **`SerialPortInputStream`**
  - A reader thread waits in `poll()`, appends what `read()` returns to a buffer, and notifies
    `ChangeListener`s on the message thread (`NOTIFY_ALWAYS`, `NOTIFY_ON_CHAR`, `NOTIFY_OFF`).
  - While a listener runs, `getTotalLength()` and `read()` on the message thread see only the
    bytes that had arrived when the callback started. Newer bytes trigger another callback.
  - It stops, and reports, when the other end hangs up. `close()` wakes it through a pipe.
- **`SerialPortOutputStream`** is unbuffered. `write()` waits with `poll(POLLOUT)`, up to 1 s per
  stall, and `flush()` does nothing.
- **`getSerialPortPaths()`** lists `/dev/ttyS*`, `ttyUSB*`, `ttyACM*`, `ttyAMA*`, `ttymxc*`,
  `ttyLP*` and `rfcomm*` on Linux, and `/dev/cu.*` and `tty.*` on macOS.
- **Errors and state changes** go to the `DebugFunction` passed to the constructor.
  `input_events` passes one that logs them.

### 8.7 Testing without a unit

**Unit tests.** Both are console targets under `Kick/tests`, built unless
`-DKICK_LINK_TESTS=OFF`, and run in CI:

```sh
cmake --build Kick/build --target kick_protocol_roundtrip kick_serialport_pty_test
ctest --test-dir Kick/build --output-on-failure -R '^kick_'
```

- `kick_protocol_roundtrip` covers:
  - every packet type in both directions, including the 17-, 32- and 33-byte pad sets;
  - the frame layout, and the CRC against an independent implementation;
  - the reader cases in 8.4.
- `kick_serialport_pty_test` runs `juce_serialport` against a `posix_openpt` pair:
  - open and configuration, notifications on the message thread, the fixed length inside a
    callback;
  - a 256 KiB write, close and reopen, and the other end hanging up.

  A Linux pty forces 8 data bits and no parity, so the test checks only the rate and stop bits
  there.

**The whole app against a fake surface.** `Kick/tests/fake_mcu.py` (Python 3, standard library
only) plays the microcontroller. It answers the handshake, sends the keys, pads, knobs and slider
moves listed in `--steps`, and logs every frame with a hex dump. `--capture PREFIX` writes the raw
bytes of each direction to files. It reads the ids from this tree.

```sh
Kick/tests/fake_mcu.py --pty --capture /tmp/kick
#   FakeMCU: [   0.003s] listening on /dev/ttys012
cd Kick/build && KICK_SERIAL_DEVICE=/dev/ttys012 ./Kick.app/Contents/MacOS/Kick
```

`tap:KNOB_ENTER` in `--steps` opens the project selected on the start screen. After that, keys such as STOP,
BANK_B and HALF_LEVEL get their LED frames back. On Linux, `--link /dev/ttymxc0` also makes the
pty appear as the unit's own port, so the app connects with `KICK_SERIAL_DEVICE` unset.

Kick allows one instance per machine (`moreThanOneInstanceAllowed()` is false). A second one
started while another runs exits at once, with no log.

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
| `KICK_SCREEN_MIRROR=<unix socket path>` | The main window gets no native window: a headless peer renders it (popups and bubbles included) and serves it over RFB ("VNC") on that socket, and the viewer's pointer becomes touches (9.5). For an emulator that shows the screen inside its panel, and for checks with the computer's screen locked | `modules/test_control` (`kick_ScreenMirror.*`), used by `HomeWindow::createNewPeer` in `apps/kick/Main.cpp` | Not created: the window is the normal native one, created as before; no socket, nothing logged |
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
| `touch X Y` | The same as a real touch: a button-down and, 80 ms later, a button-up handed to the window's `ComponentPeer`, so JUCE dispatches it to the component, its mouse listeners and its parents' (list rows). With a native window it needs the window to be the topmost one there (it floats the window above the others for the touch, and refuses while the screen is locked); with the screen mirror (9.5) it always works |
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

### 9.5 The screen mirror

`KICK_SCREEN_MIRROR=<unix socket path>` lets another program show the app's screen and touch it,
with nothing of the app on the computer's desktop. An emulator uses it to draw the screen inside
its front panel, in the display cut-out. Without the variable nothing changes: `HomeWindow` is
created and put on the desktop exactly as before, and none of this code runs.

With it, `HomeWindow::createNewPeer` returns a headless `ComponentPeer`
(`testcontrol::ScreenMirror`, `modules/test_control/source/kick_ScreenMirror.*`) instead of a
native window. JUCE neither knows nor cares: it paints into that peer and dispatches its input as
it does for a native window.

- **Painting.** `repaint()` on the window or anything on it (popups, the spinner and bubbles are
  its children, 9.4) reaches the peer, which collects the areas and renders them with JUCE's
  software renderer, the renderer the unit uses, at most 30 times a second. It paints only while a
  viewer is connected; a new viewer first gets the whole screen.
- **Serving.** The peer runs a minimal RFB 3.8 server (RFC 6143) on the unix socket: security
  type None, raw encoding, only the rectangles that changed, one viewer at a time (a new one
  replaces the old). The socket file is created with mode 0600 and removed on exit; there is no
  network listener. Viewers may ask for any 32-bit true-colour pixel format.
- **Touches.** The viewer's pointer events become mouse events on the peer, through
  `ComponentPeer::handleMouseEvent` as a native window's are: button 1 down, moves while it is
  down, button 1 up. Moves with no button held (a viewer's hover) are ignored, since a touch
  panel has no hover. JUCE then dispatches them to the component under the point, its mouse
  listeners and its parents' listeners (list rows, drags, sliders), with its own focus handling,
  as for a finger on the unit's screen. The peer reports itself focused (it is the only window),
  so JUCE gives keyboard focus to touched controls. Keyboard events from the viewer are ignored:
  keys come from the control surface.
- **No desktop dependency.** Nothing asks the window server anything, so painting, touches, the
  test-control module's `touch` and `shot` work with the computer's screen locked. On macOS the
  app's Dock icon is hidden. On Linux the window is not made the kiosk component (there is no
  display to fill).

Log lines: `ScreenMirror: serving the screen on <path> (...)`, `viewer connected (1024x600)`,
`touch down <x> <y> on <component>` / `touch up ...`, and `viewer gone (<why>) after <n> s:
<updates> updates (<n>/s), <n> MB`. If the socket cannot be created (path too long, a
non-socket file in the way) it logs `ScreenMirror: cannot serve the screen on ...; using a normal
window` and the app runs with its native window. Not available on Windows.

Any RFB viewer that can connect to a unix socket and accepts no authentication can be the
viewer; the emulator's panel is one (`kickem-panel --screen embedded --screen-source
unix:<path>`, `KickEm-Tools/KickEmulator`).
