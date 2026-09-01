# Backlog

Priorities: P0 = fixes a real failure seen on hardware · P1 = robustness /
agent ergonomics · P2 = new capability · P3 = infra. Effort: S < 1h-ish,
M = an afternoon, L = larger.

## Open

| # | Item | Effort | Why |
|---|------|--------|-----|
| 11b | **Wi-Fi validation** — transport reporting is real now, but only USB is hardware-tested | M | Needs camera Wi-Fi pairing. |
| 16 | **Absolute focus positioning** (`FocusPositionSetting`) | M | near/far nudges work; absolute would be better for agents. |

## Done (this effort)

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
