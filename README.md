# sonycam

Agent-friendly CLI for controlling Sony Alpha cameras (built for the a7C II /
ILCE-7CM2) through Sony's official [Camera Remote SDK](https://support.d-imaging.sony.co.jp/app/sdk/en/index.html).

A small daemon (`sonycamd`) holds the camera connection open; the `sonycam`
CLI talks to it over a unix socket, so repeated commands don't pay the
multi-second SDK reconnect cost. `--json` output makes it directly usable by
coding agents via shell.

```
sonycam status                    # connection state + model
sonycam info                      # gear: body/lens model, serials, firmware
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

These are the exact steps used to bring this up on an Apple Silicon Mac with
an a7C II (SDK v2.02.00):

1. Download the Camera Remote SDK from
   [Sony's SDK page](https://support.d-imaging.sony.co.jp/app/sdk/en/index.html)
   (free, requires accepting Sony's license). **The SDK cannot be
   redistributed — do not commit it to this repository.**
2. Unpack the download. Inside you'll find PDFs, the API reference, and the
   sample-app archives — the one you need is `RemoteCli.zip`:

   ```sh
   mkdir -p /tmp/sonycam-crsdk && cd /tmp/sonycam-crsdk
   unzip ~/Downloads/Camera_Remote_SDK_*.zip
   unzip RemoteCli.zip     # -> RemoteCli/ with app/CRSDK headers
                           #    and external/crsdk/libCr_Core.dylib (+ CrAdapter/)
   ```
3. Point the build at the extracted `RemoteCli` directory:

   ```sh
   cmake -B build -DSONY_SDK_DIR=/tmp/sonycam-crsdk/RemoteCli
   cmake --build build -j
   ```

   `SONY_SDK_DIR` accepts either Sony's `RemoteCli`/`SimpleCli` sample root
   (headers in `app/CRSDK`, libs in `external/crsdk`) or a flat SDK tree.
   CMake fails with a clear message if it can't find the headers or
   `libCr_Core`. On macOS the build stages the SDK dylibs and the
   `CrAdapter/` plugin directory next to the binaries automatically.

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

## Troubleshooting

Everything below was hit (and solved) during real hardware bring-up on macOS
with an a7C II.

**`no camera found`** — check, in order:
- Camera is on and not asleep; USB cable carries data (not charge-only).
- `Menu > Setup > USB > USB Connection Mode > PC Remote` is set.
- macOS actually sees it: `ioreg -p IOUSB | grep -i ILCE` should list the
  camera.

**macOS steals the camera (`ptpcamerad`)** — macOS launches `ptpcamerad`
for any imaging USB device and it grabs the PTP session before the SDK can.
`sonycamd` handles this automatically during connect: it suppresses
`ptpcamerad` and force re-enumerates the Sony USB device. The startup line
`sonycamd: re-enumerated 1 Sony USB device(s)` is normal, not an error.

**First command is slow (~6 s)** — expected: it spawns the daemon and pays
the SDK connect cost once. Subsequent commands are instant.

**First command hangs forever when piped** — known bug: on a cold start the
auto-spawned daemon inherits the client's pipe, so `sonycam status | head`
or `$(sonycam --json props)` never sees EOF. Workaround: run a bare
`sonycam status` first, then pipe freely.

**`capture` times out ("no image arrived")** — two causes, both camera-side:
- The mode dial is in a movie/S&Q position (`props` shows a hex
  `exposure_program` such as `0x8053`). Fix: `sonycam set exposure_program
  manual`. This override reverts to the physical dial on every reconnect.
- Autofocus can't lock (af_c/af_s in low light) so the camera refuses to
  release. Fix: `sonycam set focus_mode mf` and retry.

**`aperture is not writable in the current camera mode`** — in A/M modes
this means the lens's physical aperture ring is not on its "A" position
(check the Iris Lock switch too). Run `sonycam info` to identify the lens.
No software fix exists; turn the ring.

**`drive_mode 0xffffffff` / read-only exposure_comp** — normal in movie
modes; the camera genuinely doesn't expose them there. Switch to a still
mode.

**Sets fail right after a capture** — the camera locks all properties for
~2–3 s while it stores the shot. Wait and retry.

**Stuck or weird daemon state** — `sonycam daemon stop`, then run any
command to restart it. Settings changed remotely (exposure_program, etc.)
revert to the physical dial on reconnect.

**Unknown hex values in `props`** — the camera reported a value outside the
CLI's name tables (common for scene/movie modes). They're valid: pass the
hex back to `set` to select them.

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

- **Validated on real hardware**: a7C II (ILCE-7CM2, firmware 1.02) with the
  FE 24-70mm F2.8 GM II over USB on Apple Silicon macOS. Full round trip
  works: connect, props, get/set (incl. aperture with the lens ring on "A"),
  capture with image download, live view, gear identification.
- Known bug: the auto-spawned daemon inherits the client's stdio, so the
  *first* command after a cold start hangs if piped (see Troubleshooting).
- Not exposed yet (SDK supports them): remote focus drive, power-zoom
  control, movie record start/stop, file format selection.
- Value codecs for shutter/ISO/EV follow the SDK header conventions; exact
  accepted values depend on the camera mode (e.g. aperture is not writable
  in S mode, most exposure props are locked in full Auto).
