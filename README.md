# sonycam

Agent-friendly CLI for controlling Sony Alpha cameras (built for the a7C II /
ILCE-7CM2) through Sony's official [Camera Remote SDK](https://support.d-imaging.sony.co.jp/app/sdk/en/index.html).

A small daemon (`sonycamd`) holds the camera connection open; the `sonycam`
CLI talks to it over a unix socket, so repeated commands don't pay the
multi-second SDK reconnect cost. `--json` output makes it directly usable by
coding agents via shell.

```
sonycam status                    # connection state + model
sonycam props                     # all properties with current values
sonycam get iso
sonycam set iso 800
sonycam set aperture 2.8
sonycam set shutter_speed 1/250
sonycam set white_balance daylight
sonycam capture --dir ~/photos    # trigger the shutter
sonycam liveview frame.jpg        # save one live-view frame
sonycam --json props              # machine-readable output for agents
sonycam daemon stop
```

Supported properties: `iso`, `aperture`, `shutter_speed`, `exposure_comp`,
`exposure_program`, `white_balance`, `focus_mode`, `focus_area`, `drive_mode`,
`priority_key`.

## Build

Requires CMake ≥ 3.16 and a C++17 compiler. Two modes:

### Fake mode (no SDK, no camera - development & CI)

```sh
cmake -B build
cmake --build build -j
ctest --test-dir build --output-on-failure   # runs the integration tests
build/sonycam --fake props                   # simulated a7C II
```

### Real camera (macOS / Linux)

1. Download the Camera Remote SDK from
   [Sony's SDK page](https://support.d-imaging.sony.co.jp/app/sdk/en/index.html)
   (free, requires accepting Sony's license). **The SDK cannot be
   redistributed — do not commit it to this repository.**
2. Unpack it anywhere, e.g. `~/CrSDK`. The directory must contain the
   `CRSDK/` headers and the `libCr_Core` library (`.dylib` on macOS, `.so` on
   Linux, typically under `external/crsdk/`).
3. Build against it:

```sh
cmake -B build -DSONY_SDK_DIR=$HOME/CrSDK
cmake --build build -j
```

On Linux you may need the udev rule from the SDK docs so the camera is
accessible without root.

### Camera setup (a7C II)

- Update the camera firmware first.
- USB: connect the cable, then `Menu > Setup > USB > USB Connection Mode >
  PC Remote`.
- Wi-Fi: `Menu > Network > Cnct./PC Remote > PC Remote Function > On` and pair
  via access-point mode.
- The daemon automatically requests `priority_key = pc_remote` on connect so
  the camera accepts remote setting changes.

### Hardware smoke test

With the camera connected:

```sh
scripts/smoke_test.sh build/sonycam
```

It walks status → props → get/set ISO → live view → capture and prints
pass/fail per step.

## Architecture

```
sonycam (CLI) --JSON lines over unix socket--> sonycamd (daemon)
                                                 ├── FakeBackend   (always built)
                                                 └── CrsdkBackend  (built with -DSONY_SDK_DIR)
                                                        └── Sony Camera Remote SDK
```

- `src/backend.hpp` — backend interface.
- `src/fake_backend.cpp` — stateful simulated camera (same property surface).
- `src/crsdk_backend.cpp` — real SDK integration: enumerate → connect →
  priority key → property get/set with human-readable value codecs
  (`1/250`, `f/2.8`, `iso auto`) → capture → live view.
- `src/daemon.cpp` / `src/cli.cpp` — protocol plumbing.

Reference material (not dependencies): [crsdk.app property docs](https://crsdk.app/web-api/properties),
[SonShell](https://github.com/goudvuur/sonshell), [TetherMoon](https://github.com/SpaceDLFactory/TetherMoon).

## Status / known limitations

- Built and tested in fake mode. **The CrSDK backend compiles against SDK
  v2 headers but has not yet been validated against a physical a7C II** —
  run the smoke test and report failures.
- Captured images: `capture` triggers the shutter; automatic download of the
  resulting file is not wired up yet (the SDK delivers it asynchronously).
- Value codecs for shutter/ISO/EV follow the SDK header conventions; exact
  accepted values depend on the camera mode (e.g. aperture is not writable
  in S mode, most exposure props are locked in full Auto).
