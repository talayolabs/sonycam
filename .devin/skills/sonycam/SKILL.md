---
name: sonycam
description: Control a Sony camera (shoot, live view, read/change settings) with the sonycam CLI
argument-hint: "<what to do with the camera>"
allowed-tools:
  - read
  - exec
  - grep
  - glob
permissions:
  allow:
    - Exec(./build/sonycam)
    - Exec(sonycam)
---

You are operating a real Sony camera through the `sonycam` CLI. The binary
lives at `./build/sonycam` in this repo (or `sonycam` if installed on PATH).
A background daemon (`sonycamd`) holds the camera connection; the first CLI
call auto-starts it (~6s including USB connect). All later calls are fast.

## Commands

```
sonycam status                    connection state and camera model
sonycam info                      identify gear: body/lens model, serials, firmware,
                                  and whether the lens supports remote (power) zoom
sonycam props                     all properties with values (+ per-mode writability)
sonycam get <prop>                one property, includes valid choices
sonycam set <prop> <value>        change a property, verifies the camera applied it
sonycam capture [--dir DIR]       fire the shutter, downloads the image, prints its path
sonycam liveview <out.jpg>        save one live-view frame (fast, no shutter)
sonycam connect | disconnect      manage the camera connection
sonycam daemon stop               stop the background daemon
```

Properties: `iso aperture shutter_speed exposure_comp exposure_program
white_balance focus_mode focus_area drive_mode priority_key`

Add `--json` for machine-readable output. Exit codes: 0 ok, 1 camera error,
2 usage, 3 daemon unreachable.

## Critical rules

1. **Never pipe or capture the output of the first command after a cold
   start.** The auto-spawned daemon inherits the pipe and the command hangs
   forever. Warm up first, then pipe:
   ```
   sonycam status          # bare, warms up the daemon
   sonycam --json props    # now safe to pipe/capture
   ```

2. **Start every session with `sonycam props`** to learn the camera's current
   mode and which properties are writable. `(read-only)` markers are
   per-mode, not permanent.

3. **`capture` requires a still-image mode.** If the physical dial is in a
   movie position (exposure_program shows a hex like `0x8053`), run
   `sonycam set exposure_program manual` (or `aperture_priority`, etc.)
   first. This override resets whenever the daemon reconnects, so re-check
   after any disconnect or daemon restart.

4. **Autofocus can block the shutter.** In af_c/af_s with low light or low
   contrast, the camera refuses to release and capture fails with a timeout.
   Fall back to `sonycam set focus_mode mf` and retry.

5. **Wait ~3s after a capture** before setting properties; the camera
   briefly reports everything as "not writable" while it stores the shot.

## Value formats

| Property | Examples |
|---|---|
| iso | `auto`, `400`, `6400` |
| aperture | `5.6`, `f/2.8` |
| shutter_speed | `1/250`, `2"`, `bulb` |
| exposure_comp | `+0.7`, `-1.0` |
| exposure_program | `manual`, `program_auto`, `aperture_priority`, `shutter_priority`, `auto` |
| white_balance | `auto`, `daylight`, `cloudy`, `tungsten`, `color_temp` |
| focus_mode | `af_s`, `af_c`, `af_a`, `dmf`, `mf` |
| focus_area | `wide`, `zone`, `center`, `spot_m`, `expand_spot` |

`get <prop>` always prints the exact `choices:` accepted by the current
camera — trust that list over this table. Values the CLI cannot name are
shown as hex (e.g. `0x8053` = movie-manual); you can pass raw hex back to
`set` to select them.

## Troubleshooting

- `no camera found`: camera off, asleep, or USB unplugged. Ask the user to
  check the cable and that the camera is on.
- `aperture is not writable`: either an auto exposure mode, or the lens has
  a physical aperture ring not set to its "A" position (hardware; cannot be
  fixed in software). Run `sonycam info` to identify the lens and check
  whether it has an aperture ring.
- `remote_zoom no` in `sonycam info`: the lens has a mechanical zoom ring
  that cannot be driven remotely; only power-zoom (PZ) lenses support it.
- `drive_mode 0xffffffff`: normal in movie modes; switch to a still mode.
- Stuck or weird daemon state: `sonycam daemon stop`, then retry (next
  command restarts it).
- No hardware attached: use `sonycam --fake <cmd>` for a simulated camera
  (also used by the test suite: `ctest --test-dir build`).

## Example: take a photo, whatever state the camera is in

```
sonycam status                              # warm up + verify connection
sonycam props                               # inspect mode
sonycam set exposure_program manual         # only if in a movie/hex mode
sonycam capture --dir ./shots               # prints: captured: ./shots/DSC0xxxx.JPG
# on "no image arrived" timeout:
sonycam set focus_mode mf && sonycam capture --dir ./shots
```
