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
sonycam focus af                  autofocus (half-press), waits for lock, reports state
sonycam focus near|far [N]        manual-focus nudge N steps (requires focus_mode mf)
sonycam focus position            read absolute focus position + valid range
sonycam focus position <V>        drive the lens to position V (requires mf);
                                  waits for arrival and reports the final position
sonycam focus at X Y              move the AF area to X,Y (0-1 fractions of the
                                  frame) and lock — "focus there" for vision loops
sonycam focus save|recall <slot>  in-camera focus position memories (verified:
                                  recall drives the lens back to the saved spot)
sonycam focus status              current focus indication (unlocked/focused/tracking)
sonycam record start|stop|status  movie recording (camera must be in a movie mode)
sonycam zoom in|out [MS] | stop   power zoom (fails cleanly on mechanical lenses)
sonycam preset save|load <file>   save/restore the FULL camera configuration
sonycam files list                list photos/videos on the memory card
sonycam files pull <name> [--dir DIR]
                                  download any card file, RAW and video included
sonycam wb capture                meter custom WB at frame center (requires
                                  'set white_balance custom_1' first + light)
sonycam capture [--dir DIR] [--count N] [--interval SECS]
                                  fire the shutter; N shots SECS apart
sonycam liveview <out.jpg>        save one live-view frame (fast, no shutter)
sonycam liveview <out.jpg> --follow [--frames N]
                                  stream frames (atomic overwrite) until ctrl-c/N
sonycam --version                 print the CLI version
sonycam connect | disconnect      manage the camera connection
sonycam daemon stop               stop the background daemon
```

Properties: `iso aperture shutter_speed exposure_comp exposure_program
white_balance color_temp file_format image_quality movie_format movie_fps
movie_quality picture_profile subject_recognition recognition_target
eye_select af_transition_speed af_shift_sensitivity steadyshot_movie
zoom_range touch_operation auto_power_off_temp focus_mode focus_area
drive_mode priority_key`

Add `--json` for machine-readable output. Exit codes: 0 ok, 1 camera error,
2 usage, 3 daemon unreachable.

## Session start: run this ONCE before anything else

Run these three commands in order and build a mental model of the hardware
before taking any other action:

```
sonycam status      # 1. start the daemon + confirm the camera is connected
sonycam info        # 2. identify the gear
sonycam props       # 3. learn current mode and what is writable right now
```

The first command pays a ~6s daemon-spawn + connect cost; everything after
is instant. The daemon logs to `<socket>.log` (default `~/.sonycam.sock.log`)
— read it when the daemon misbehaves.

Interpret the results and remember them for the whole session:

- **`info` → lens**: look up whether this lens has a physical aperture ring
  and note `remote_zoom`. This tells you up front which failures are
  hardware limits you must not retry:
  - `remote_zoom no` → zoom is a mechanical ring; never attempt to zoom,
    ask the user to turn it instead.
  - Lens has an aperture ring and `props` shows `aperture (read-only)` in a
    mode where it should be writable (A or M) → the ring is off its "A"
    position; ask the user to turn the ring to "A" (check the Iris Lock
    switch). No software action can fix this.
- **`props` → exposure_program**: a movie/S&Q value (`movie_m`, `sq_auto`,
  ...) means the physical dial is in a movie position. `capture` needs a
  still mode: run `sonycam set exposure_program manual` (or
  `aperture_priority`, ...). This override is per-connection — it silently
  reverts to the dial whenever the daemon reconnects, so re-run the check
  after any disconnect, daemon restart, or USB replug.
- **`props` → `(read-only)` markers** are per-mode, not permanent. Re-read
  after changing exposure_program.

Report the identified gear (body, lens, firmware) to the user at the start
so hardware limitations are understood by everyone.

## Hard limitations — things this tool can NOT do

Do not attempt these, do not retry them, and do not promise them to the
user. When one is requested, explain the limitation and offer the listed
alternative.

1. **Register or recall the camera's Memory slots (M1/M2/M3 on the dial).**
   Sony's Camera Remote SDK simply does not expose it (verified against the
   full per-model API matrix, latest SDK version). Alternative:
   `sonycam preset save/load <file>` snapshots and restores the ENTIRE
   camera configuration to/from a file on this computer — same outcome,
   unlimited slots. Registering M1/M2/M3 themselves must be done in the
   camera menu by a human.
2. **Zoom or change focal length on a mechanical-zoom lens**
   (`remote_zoom no` in `info`). There is no motor; a human must turn the
   ring. Only power-zoom (PZ) lenses can zoom remotely.
3. **Control aperture while the lens's aperture ring is off "A".** The
   physical ring always wins; ask the human to move it to "A".
4. **Download RAW/video as part of `capture`.** capture only receives the
   JPEG rendition. RAWs and clips ARE retrievable afterwards with
   `files pull <name>`, but be aware it switches the camera out of remote
   mode for the duration (~15s overhead + transfer time; no shooting
   meanwhile) and reconnects when done.
5. **Make remote overrides survive a reconnect.** exposure_program and
   friends silently revert to the physical dial after any disconnect,
   replug, or camera reboot. Re-apply them (or use `preset load`).
6. **Confirm dialogs on the camera's screen.** e.g. the USB-connection-mode
   prompt after a `preset load` reboot needs a human (or pin the camera's
   USB Connection Mode menu to "PC Remote" beforehand).
7. **Power the camera ON.** Once off (or after battery pull), USB gives us
   nothing; a human must flip the switch.
8. **Set Auto ISO limits or Log shooting on this body.** The a7C II rejects
   `iso_auto_min`/`iso_auto_max`/`log_shooting` over remote even though the
   CLI wires them; they must be set in the camera menu (a preset file does
   capture them).
9. **Physical acts**: mounting lenses, inserting cards/batteries, moving
   the camera, removing the lens cap.

## Critical rules during operation

1. **Focus before you shoot.** Run `sonycam focus af` first: it half-presses,
   waits for lock, and tells you the outcome. If it fails (low light / low
   contrast), switch to `set focus_mode mf` and use `focus near/far [N]` with
   liveview frames to focus by eye. A capture attempted with unlocked AF
   fails with a timeout. `focus af` only locks in still modes.

2. **Restore what you change.** If you override exposure_program, focus_mode
   etc. to get a shot, put the user's original values back afterwards
   (record them from the session-start `props` output).

## Value formats

| Property | Examples |
|---|---|
| iso | `auto`, `400`, `6400` |
| aperture | `5.6`, `f/2.8` |
| shutter_speed | `1/250`, `2"`, `bulb` |
| exposure_comp | `+0.7`, `-1.0` |
| exposure_program | `manual`, `program_auto`, `aperture_priority`, `shutter_priority`, `auto` |
| white_balance | `auto`, `daylight`, `cloudy`, `tungsten`, `color_temp` |
| color_temp | `5500K` or `5500` (kelvin; only applies when white_balance is `color_temp`) |
| file_format | `jpeg`, `raw`, `raw+jpeg`, `heif` |
| image_quality | `light`, `standard`, `fine`, `extra_fine` |
| focus_mode | `af_s`, `af_c`, `af_a`, `dmf`, `mf` |
| focus_area | `wide`, `zone`, `center`, `spot_m`, `expand_spot` |

`get <prop>` always prints the exact `choices:` accepted by the current
camera — trust that list over this table. Movie/S&Q exposure programs are
named (`movie_m`, `sq_auto`, `interval_p`, ...). `-` means the camera
reports no value in the current mode. Rare values the CLI cannot name are
shown as hex and can be passed back to `set` verbatim.

## Troubleshooting

- `no camera found`: camera off, asleep, or USB unplugged. Ask the user to
  check the cable and that the camera is on. After a replug the next
  command reconnects automatically (~6s); an explicit `disconnect` is
  sticky and needs `connect`. Remember the camera reverts to its physical
  dial mode after any reconnect.
- `the camera refused the shutter release`: definitive — the shutter did
  not fire. Movie mode or AF-block; fix the mode / focus and retry.
- `aperture is not writable`: either an auto exposure mode, or the lens has
  a physical aperture ring not set to its "A" position (hardware; cannot be
  fixed in software). Run `sonycam info` to identify the lens and check
  whether it has an aperture ring.
- `remote_zoom no` in `sonycam info`: the lens has a mechanical zoom ring
  that cannot be driven remotely; only power-zoom (PZ) lenses support it.
- `drive_mode -` (read-only): normal in movie modes; switch to a still mode.
- `focus af` fails with "did not lock": add light, aim at higher contrast,
  try `set focus_area center`, or fall back to mf + `focus near/far` while
  checking liveview frames.
- `focus position` is the best MF workflow: read the current position once,
  then binary-search positions with liveview frames to focus by eye.
  Lower values = nearer. "lens stopped at X" means a physical focus limit.
- Ranges print as `min..max step X` in choices (e.g. color_temp
  `2500K..9900K step 100K`).
- Even with `file_format raw` or `raw+jpeg`, the file downloaded by
  `capture` is the JPEG rendition; RAW files stay on the memory card.
- `file_format`/`image_quality` are read-only in movie modes.
- **Presets**: `preset save/load` needs a still exposure mode (fails with
  CrError 0x8402 in movie modes). After `preset load` the camera REBOOTS
  (~30-60s off the USB bus) and, unless its USB Connection Mode menu is
  pinned to "PC Remote", it asks on-screen for the connection mode — a
  human must confirm it. Warn the user before loading.
- Movie fps choices follow the camera's NTSC/PAL setting (NTSC: 24p/30p/
  60p; PAL: 25p/50p/100p) — always trust `get movie_fps` choices.
- `files list/pull` reconnect the camera in contents mode: expect ~15s
  before results and no remote control until the command returns. Pull
  waits up to 5 minutes (large videos take a while over USB).
- `wb capture` needs `white_balance custom_1` selected first, a neutral
  target, and decent light; the camera legitimately rejects bad readings.
- `focus at` works with any AF area on the a7C II (locks as `tracking`);
  if the camera rejects the position, try `set focus_area spot_m`.
- `iso_auto_min`/`iso_auto_max` and `log_shooting` exist in the CLI but the
  a7C II rejects them over remote (absent from `props`); set Auto ISO
  limits on the camera body.
- Stuck or weird daemon state: `sonycam daemon stop`, then retry (next
  command restarts it).
- No hardware attached: use `sonycam --fake <cmd>` for a simulated camera
  (also used by the test suite: `ctest --test-dir build`).

## Example: take a photo, whatever state the camera is in

```
sonycam status && sonycam info && sonycam props   # session start (see above)
sonycam set exposure_program manual               # only if in a movie/sq mode
sonycam focus af                                  # verify focus before shooting
sonycam capture --dir ./shots                     # captured: ./shots/DSC0xxxx.JPG
# if focus af failed to lock:
sonycam set focus_mode mf
sonycam focus near 10                             # + liveview to check by eye
sonycam capture --dir ./shots
# afterwards, restore the user's original settings recorded at session start
```
