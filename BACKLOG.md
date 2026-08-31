# Backlog

Priorities: P0 = fixes a real failure seen on hardware · P1 = robustness /
agent ergonomics · P2 = new capability · P3 = infra. Effort: S < 1h-ish,
M = an afternoon, L = larger.

## P0 — unblocks failures we actually hit

| # | Item | Effort | Why |
|---|------|--------|-----|
| 1 | **Remote focus control** — `sonycam focus` (AF half-press trigger, MF near/far nudges via SDK focus operation) | M | AF-block is the #1 capture failure; today the only remedy is switching to `mf` blind. Lets agents focus deliberately. |
| 2 | **Movie record start/stop** — `sonycam record start\|stop` | S-M | The camera's dial lives in movie mode; the CLI can't do the one thing that mode is for. SDK: `CrCommandId_MovieRecord`. |

## P1 — robustness & agent ergonomics

| # | Item | Effort | Why |
|---|------|--------|-----|
| 3 | **Name remaining enum values** — movie/S&Q exposure programs (`0x8053` → `movie_manual`), extra WB modes, `0xffffffff` → `-` | S | Agents currently juggle raw hex; error-prone. |
| 4 | **Busy-retry on transient "not writable"** — after capture/mode change the camera locks props ~2-3s; `set` should retry briefly instead of erroring | S | Removes the "wait ~3s" footgun from the skill. |
| 5 | **USB unplug/replug recovery** — verify daemon behavior on `OnDisconnected`, auto-reconnect on next command | M | Untested path; needs physical cable pull (requires user). |
| 6 | **Readiness wait after mode change** — replace capture's blind 3x release retry with polling a readiness signal if one exists | M | Current retry works but is a heuristic. |

## P2 — new capabilities

| # | Item | Effort | Why |
|---|------|--------|-----|
| 7 | **File format control** — `set file_format raw\|jpeg\|raw+jpeg`, JPEG quality | S | Capture downloads whatever the camera is set to; agents can't choose. |
| 8 | **Custom WB color temperature** — set kelvin value when `white_balance color_temp` | S | Mode selectable today, value isn't. |
| 9 | **Liveview streaming** — `liveview --follow` writing frames continuously (agent vision loops) | M | Single-frame works; loops currently shell out repeatedly. |
| 10 | **Power-zoom control** | S | No-op for the current GM II lens (`remote_zoom no`); useful for PZ lenses. |
| 11 | **Real transport reporting + Wi-Fi validation** — `status` hardcodes `usb/net`; test Wi-Fi connect path | M | Only USB validated. |
| 12 | **Interval / burst capture** — `capture --count N --interval S` | S | Timelapse-style agent workflows. |

## P3 — infra & housekeeping

| # | Item | Effort | Why |
|---|------|--------|-----|
| 13 | **Push branch + open PR** — 9+ local commits on `devin/1788195067-initial-cli` | S | Nothing is backed up remotely. |
| 14 | **CI** — GitHub Actions: fake-mode build + tests (macOS + Linux) | S | Tests only run locally today. |
| 15 | **`cmake --install` target / packaging** — put `sonycam`+`sonycamd` on PATH properly | S | Currently run from `build/`. |

## Done (this effort)

- Real-SDK build on macOS (layout detection, dylib staging, Gatekeeper docs)
- ptpcamerad suppression + USB re-enumeration workaround
- Capture image download (OnCompleteDownload, store-destination, release retry)
- `sonycam info` gear identification
- Daemon stdio detach (pipe-safe cold start) + `<socket>.log`
- Agent skill (`.devin/skills/sonycam`) + README install/troubleshooting docs
- Hardware-validated: a7C II + FE 24-70mm F2.8 GM II, full round trip
