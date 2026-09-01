#!/usr/bin/env bash
# Integration tests: CLI <-> daemon protocol against the fake backend.
# Usage: tests/run_tests.sh /path/to/sonycam
set -u

SONYCAM="${1:?usage: run_tests.sh /path/to/sonycam}"
WORK="$(mktemp -d)"
export SONYCAM_SOCKET="$WORK/test.sock"
export SONYCAM_FAKE=1

PASS=0
FAIL=0

cleanup() {
  "$SONYCAM" daemon stop >/dev/null 2>&1
  rm -rf "$WORK"
}
trap cleanup EXIT

check() {
  local desc="$1"; shift
  if "$@" >/dev/null 2>&1; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    echo "FAIL: $desc  ($*)" >&2
  fi
}

check_fails() {
  local desc="$1"; shift
  if "$@" >/dev/null 2>&1; then
    FAIL=$((FAIL + 1))
    echo "FAIL (expected error): $desc  ($*)" >&2
  else
    PASS=$((PASS + 1))
  fi
}

check_output() {
  local desc="$1" expected="$2"; shift 2
  local out
  out="$("$@" 2>/dev/null)"
  if [ "$out" = "$expected" ]; then
    PASS=$((PASS + 1))
  else
    FAIL=$((FAIL + 1))
    echo "FAIL: $desc  (got: '$out', want: '$expected')" >&2
  fi
}

# --- basic lifecycle (first command auto-starts the daemon) ---
check "status" "$SONYCAM" status
check "props lists properties" "$SONYCAM" props
check "connect is idempotent" "$SONYCAM" connect
check_output "info identifies gear" \
  "lens               FAKE FE 28-70mm F3.5-5.6 OSS" \
  sh -c "'$SONYCAM' info | grep '^lens '"

# --- get/set round trips ---
check "get iso" "$SONYCAM" get iso
check "set iso 800" "$SONYCAM" set iso 800
check_output "iso persisted across invocations" \
  '{"ok":true,"result":{"name":"iso","value":"800"}}' \
  python3 -c "
import json, subprocess, sys
out = subprocess.run(['$SONYCAM', '--json', 'get', 'iso'],
                     capture_output=True, text=True).stdout
d = json.loads(out)
d['result'] = {'name': d['result']['name'], 'value': d['result']['value']}
print(json.dumps({'ok': d['ok'], 'result': d['result']}, separators=(',', ':')))
"
check "set aperture 4.0" "$SONYCAM" set aperture 4.0
check "set shutter_speed 1/250" "$SONYCAM" set shutter_speed 1/250
check "set white_balance daylight" "$SONYCAM" set white_balance daylight
check "set exposure_comp (free numeric)" "$SONYCAM" set exposure_comp -0.7

# --- validation errors ---
check_fails "unknown property rejected" "$SONYCAM" get bogus_prop
check_fails "invalid enum value rejected" "$SONYCAM" set iso 999
check_fails "read-only property rejected" "$SONYCAM" set battery_level 50
check_fails "usage error on missing args" "$SONYCAM" set iso

# --- json output shape ---
check_output "json error shape" \
  '{"error":"unknown property: nope","ok":false}' \
  "$SONYCAM" --json get nope

# --- capture and liveview produce files ---
check "capture" "$SONYCAM" capture --dir "$WORK"
check "capture wrote a jpg" ls "$WORK"/DSC00001.JPG
check "capture creates missing save dir" "$SONYCAM" capture --dir "$WORK/new/nested"
check "nested capture wrote a jpg" ls "$WORK"/new/nested/DSC00002.JPG
check "liveview" "$SONYCAM" liveview "$WORK/frame.jpg"
check "liveview wrote a jpg" test -s "$WORK/frame.jpg"

# --- focus control ---
check "focus status" "$SONYCAM" focus status
check "set focus_mode af_s" "$SONYCAM" set focus_mode af_s
check_output "focus af locks in AF mode" "focus: focused" "$SONYCAM" focus af
check_fails "focus near refused in AF mode" "$SONYCAM" focus near
check "set focus_mode mf" "$SONYCAM" set focus_mode mf
check_output "focus near nudges in MF mode" "focus: near x3" "$SONYCAM" focus near 3
check_fails "focus af refused in MF mode" "$SONYCAM" focus af
check_fails "focus rejects bad op" "$SONYCAM" focus sideways
check "restore focus_mode af_s" "$SONYCAM" set focus_mode af_s

# --- priority key gate ---
check "set priority_key camera" "$SONYCAM" set priority_key camera
check_fails "capture refused without pc_remote" "$SONYCAM" capture --dir "$WORK"
check "set priority_key pc_remote" "$SONYCAM" set priority_key pc_remote

# --- web ui ---
UI_PORT=$(( (RANDOM % 20000) + 20000 ))
"$SONYCAM" --ui "127.0.0.1:$UI_PORT" >/dev/null 2>&1 &
UI_PID=$!
for _ in $(seq 1 50); do
  curl -sf -m 2 "http://127.0.0.1:$UI_PORT/api/status" >/dev/null 2>&1 && break
  sleep 0.1
done
check "ui serves embedded html" \
  sh -c "curl -sf -m 5 http://127.0.0.1:$UI_PORT/ | grep -q sonycam"
check "ui GET /api/props" \
  sh -c "curl -sf -m 5 http://127.0.0.1:$UI_PORT/api/props | grep -q '\"iso\"'"
check "ui POST /api/set applies" \
  sh -c "curl -sf -m 5 -X POST -d '{\"prop\":\"iso\",\"value\":\"1600\"}' http://127.0.0.1:$UI_PORT/api/set | grep -q '\"1600\"'"
check_output "ui set visible via cli" "1600" \
  sh -c "'$SONYCAM' get iso | awk 'NR==1{print \$2}'"
check "ui rejects invalid value" \
  sh -c "curl -s -m 5 -X POST -d '{\"prop\":\"iso\",\"value\":\"nope\"}' http://127.0.0.1:$UI_PORT/api/set | grep -q '\"ok\":false'"
check "ui 404s unknown path" \
  sh -c "curl -s -m 5 -o /dev/null -w '%{http_code}' http://127.0.0.1:$UI_PORT/nope | grep -q 404"
kill "$UI_PID" 2>/dev/null
wait "$UI_PID" 2>/dev/null

# --- daemon stop ---
check "daemon stop" "$SONYCAM" daemon stop

# --- auto-start works when invoked via PATH (bare command name) ---
export SONYCAM_SOCKET="$WORK/path.sock"
PATH="$(dirname "$SONYCAM"):$PATH" check "auto-start via PATH lookup" "$(basename "$SONYCAM")" status
PATH="$(dirname "$SONYCAM"):$PATH" check "daemon stop (PATH)" "$(basename "$SONYCAM")" daemon stop

# --- cold-start output must be pipe-safe (daemon must not inherit stdio) ---
# The perl wrapper aborts after 10s so a regression fails instead of hanging.
export SONYCAM_SOCKET="$WORK/pipe.sock"
check_output "cold start piped to an EOF-reader completes" \
  "connected: yes" \
  perl -e 'alarm 10; print qx($ARGV[0] status | cat | head -1)' "$SONYCAM"
check "daemon stop (pipe test 1)" "$SONYCAM" daemon stop
check_output "cold start emits only client output (no daemon stderr)" \
  "connected: yes
model:     FAKE ILCE-7CM2
transport: fake" \
  perl -e 'alarm 10; print qx($ARGV[0] status 2>&1)' "$SONYCAM"
check "daemon stop (pipe test 2)" "$SONYCAM" daemon stop

echo "passed: $PASS, failed: $FAIL"
[ "$FAIL" -eq 0 ]
