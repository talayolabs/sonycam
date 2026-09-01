# sonycam

[![CI](https://github.com/talayolabs/sonycam/actions/workflows/ci.yml/badge.svg)](https://github.com/talayolabs/sonycam/actions/workflows/ci.yml)

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
sonycam focus af                  # autofocus, waits for lock
sonycam focus near 5              # manual-focus nudge (focus_mode mf)
sonycam focus position 30000      # drive to an absolute focus position
sonycam record start              # movie recording (movie mode)
sonycam zoom in 500               # power zoom (PZ lenses)
sonycam capture --dir ~/photos    # trigger the shutter
sonycam capture --count 5 --interval 2   # burst / timelapse
sonycam liveview frame.jpg        # save one live-view frame
sonycam liveview f.jpg --follow   # stream frames until ctrl-c
sonycam --json props              # machine-readable output for agents
sonycam --ui                      # web UI at http://127.0.0.1:3000
sonycam daemon stop
```

Supported properties: `iso`, `aperture`, `shutter_speed`, `exposure_comp`,
`exposure_program`, `white_balance`, `color_temp`, `file_format`,
`image_quality`, `movie_format`, `movie_fps`, `movie_quality`,
`picture_profile`, `subject_recognition`, `recognition_target`,
`eye_select`, `af_transition_speed`, `af_shift_sensitivity`,
`steadyshot_movie`, `zoom_range`, `touch_operation`, `auto_power_off_temp`,
`focus_mode`, `focus_area`, `drive_mode`, `priority_key`.

`sonycam preset save|load <file>` snapshots/restores the entire camera
configuration (requires a still mode; the camera reboots after a load).

Install with `cmake --install build` (real-SDK builds should keep running
from `build/`, where the Sony dylibs are staged).

## Web UI

`sonycam --ui [[HOST:]PORT]` serves a self-contained web page (embedded in
the binary, no external assets) that lists every property with its current
value and valid choices. Changing a control applies it to the camera
immediately — no submit button — and the page polls so external changes
(CLI, camera dials) show up live. The page also shows a live-view feed
(continuously refreshed frames from the camera; an animated test pattern in
fake mode) and a Capture button that triggers the shutter
(`POST /api/capture`, honoring the priority-key gate).

```
sonycam --ui                  # http://127.0.0.1:3000
sonycam --ui 8080             # http://127.0.0.1:8080
sonycam --ui 0.0.0.0:3000     # listen on all interfaces (LAN access)
```

The server has no authentication; bind to `0.0.0.0` only on networks you
trust.

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

   **macOS:** Sony's dylibs are only ad-hoc signed (no notarization), so if
   the zip came from a browser, Gatekeeper's quarantine flag will make macOS
   refuse to load them ("cannot verify the developer"). Strip it once:

   ```sh
   xattr -dr com.apple.quarantine /tmp/sonycam-crsdk/RemoteCli
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

**macOS blocks the SDK dylibs ("cannot be opened because the developer
cannot be verified" / `sonycamd` dies on startup)** — the Sony SDK libraries
are ad-hoc signed, not notarized, so Gatekeeper refuses to load them while
they carry the browser-download quarantine flag. Fix:

```sh
xattr -dr com.apple.quarantine /path/to/RemoteCli   # then rebuild
```

Verify with `xattr -l .../libCr_Core.dylib` — `com.apple.quarantine` must be
gone (a leftover `com.apple.provenance` entry is harmless). If a security
dialog already appeared, you can alternatively approve it under
`System Settings > Privacy & Security > Allow Anyway`. The locally built
`sonycam`/`sonycamd` binaries are never quarantined, so they need nothing.

**macOS steals the camera (`ptpcamerad`)** — macOS launches `ptpcamerad`
for any imaging USB device and it grabs the PTP session before the SDK can.
`sonycamd` handles this automatically during connect: it suppresses
`ptpcamerad` and force re-enumerates the Sony USB device. The log line
`sonycamd: re-enumerated 1 Sony USB device(s)` is normal, not an error.

**First command is slow (~6 s)** — expected: it spawns the daemon and pays
the SDK connect cost once. Subsequent commands are instant.

**Where did the daemon output go?** — the auto-spawned daemon logs to
`<socket>.log` next to its unix socket (default `~/.sonycam.sock.log`).
Check it when the daemon fails to start or behaves oddly.

**`capture` times out ("no image arrived")** — two causes, both camera-side:
- The mode dial is in a movie/S&Q position (`props` shows an
  `exposure_program` like `movie_m`). Fix: `sonycam set exposure_program
  manual`. This override reverts to the physical dial on every reconnect.
- Autofocus can't lock (af_c/af_s in low light) so the camera refuses to
  release. Check with `sonycam focus af` first; fall back to
  `sonycam set focus_mode mf` plus `sonycam focus near/far` and retry.

**`aperture is not writable in the current camera mode`** — in A/M modes
this means the lens's physical aperture ring is not on its "A" position
(check the Iris Lock switch too). Run `sonycam info` to identify the lens.
No software fix exists; turn the ring.

**`drive_mode -` / read-only exposure_comp** — normal in movie modes; the
camera genuinely doesn't expose them there. Switch to a still mode.

**USB unplugged / camera turned off mid-session** — the daemon notices
(`status` shows disconnected) and the next command reconnects automatically
once the camera is back (~6s). An explicit `sonycam disconnect` is sticky
and requires `sonycam connect`. Settings changed remotely revert to the
physical dial position after any reconnect.

**Stuck or weird daemon state** — `sonycam daemon stop`, then run any
command to restart it.

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
- Wi-Fi: `status` reports the real transport, but only USB has been
  hardware-tested.
- Capture downloads the JPEG rendition; RAW files stay on the memory card.
- Not exposed yet (SDK supports them): FTP transfer, custom white-balance
  capture, AF area positioning by coordinates.
- Value codecs for shutter/ISO/EV follow the SDK header conventions; exact
  accepted values depend on the camera mode (e.g. aperture is not writable
  in S mode, most exposure props are locked in full Auto).
