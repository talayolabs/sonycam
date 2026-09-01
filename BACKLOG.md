# Backlog

Priorities: P0 = fixes a real failure seen on hardware · P1 = robustness /
agent ergonomics · P2 = new capability · P3 = infra. Effort: S < 1h-ish,
M = an afternoon, L = larger.

## Open

| # | Item | Effort | Why |
|---|------|--------|-----|
| 21 | **Display string lists** — camera-rendered names for PP/Creative Look etc. | S | Nicer agent output than enum guesses. |
| 22 | **Maintenance ops** — card format (full/quick), new folder, file-number reset, power off, sensor cleaning | S | Operational hygiene commands. |
| 23 | **files pull progress/multi-file** — `files pull --all --since DATE`, progress output for long video transfers | S-M | Pulls are silent for minutes on big clips. |

## Done (this effort)

- #18 Card contents: `files list` / `files pull` via ContentsTransfer mode
  switch (MTP-database retry, OnNotifyContentsTransfer completion;
  verified 34MB ARW pull)
- #17 Touch-AF: `focus at X Y` (AF_Area_Position + lock; verified locks
  as tracking at arbitrary coordinates)
- #19 In-camera focus memories: `focus save/recall <slot>` (verified:
  recall drove the lens back to the saved position)
- #20 Custom WB capture: `wb capture` (standby -> capture -> result
  warnings; requires white_balance custom_1)

- Video/AF property batch (choir-recipe coverage): movie_format/fps/quality,
  picture_profile, subject_recognition, recognition_target, eye_select,
  af_transition_speed, af_shift_sensitivity, steadyshot_movie, zoom_range,
  touch_operation, auto_power_off_temp (iso_auto_min/max + log_shooting
  wired but rejected remotely by the a7C II)
- `preset save|load` full-config snapshots via Download/UploadSettingFile
  (still-mode only; camera reboots after load)

- #16 Absolute focus positioning: `focus position [V]` with
  position-convergence waiting and stall/limit detection
- CI: bumped actions (checkout v7, upload-artifact v7, download-artifact v8)
- v0.3.0 batch: #7 file_format/image_quality props, #8 color_temp kelvin
  (+ range display `min..max step X`), #9 `liveview --follow/--frames`
  with atomic frame writes, #10 `zoom in/out/stop` (gated on
  Zoom_Operation_Status), #11 real transport reporting, #12
  `capture --count/--interval`, #15 `cmake --install` + `--version`

- #1 Remote focus control: `focus af` (S1 half-press + FocusIndication
  wait), `focus near/far [N]` MF nudges, `focus status`
- #3 Named enum values: movie/S&Q/interval exposure programs, scene modes,
  fluorescent/custom WB, more drive modes, `0xffffffff` → `-`
- #4 Busy-retry in `set` during post-capture/mode-change property locks
- #2 Movie record start/stop/status (verified: 3s clip on the a7C II)
- #5 USB unplug/replug recovery: SDK reconnecting-state tracking +
  auto-reconnect on next command; explicit disconnect stays sticky
- #6 Capture readiness via CrNotify_Captured_Event: mode-switch capture in
  ~3s, definitive "release refused" errors, no double-shot risk
- #13 Branch pushed, PR #1 merged to main
- #14 CI (build + tests, macOS + Linux) and tag-driven release workflow

- Real-SDK build on macOS (layout detection, dylib staging, Gatekeeper docs)
- ptpcamerad suppression + USB re-enumeration workaround
- Capture image download (OnCompleteDownload, store-destination, release retry)
- `sonycam info` gear identification
- Daemon stdio detach (pipe-safe cold start) + `<socket>.log`
- Agent skill (`.devin/skills/sonycam`) + README install/troubleshooting docs
- Hardware-validated: a7C II + FE 24-70mm F2.8 GM II, full round trip
