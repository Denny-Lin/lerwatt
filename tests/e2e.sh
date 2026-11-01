#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# End to end through files: lerwatt-sim provides the kernel ABI, lerwattd finds
# it by itself, controls it, and puts every knob back. Also checks crash
# recovery (kill -9, then --restore) and the "nothing to control" error.
set -euo pipefail

BUILD=${1:-build}
SIM=$BUILD/lerwatt-sim
D=$BUILD/lerwattd
WORK=$(mktemp -d)
ROOT=$WORK/root
SIM_PID=
MAXF=$ROOT/sys/devices/system/cpu/cpufreq/policy4/scaling_max_freq
JOURNAL=$ROOT/run/lerwatt/restore.journal

cleanup() {
	[ -n "$SIM_PID" ] && kill "$SIM_PID" 2>/dev/null || true
	wait 2>/dev/null || true
	rm -rf "$WORK"
}
trap cleanup EXIT

fail() { echo "e2e: FAIL: $*" >&2; exit 1; }
pass() { echo "e2e: ok - $*"; }

# Never touch a real machine from a test: if this host has cpufreq, pin the sysroot.
SYSROOT_ARGS=()
if ls /sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq >/dev/null 2>&1; then
	SYSROOT_ARGS=(--sysroot "$ROOT")
	echo "e2e: this host has real cpufreq; using --sysroot instead of auto-detection"
fi

# 0. Nothing to control: no hardware, no simulator.
mkdir -p "$WORK/empty"
if [ ${#SYSROOT_ARGS[@]} -eq 0 ]; then
	if LERWATT_SIM_ROOT=$WORK/empty "$D" --duration 1 2>"$WORK/err"; then
		fail "lerwattd ran without hardware or simulator"
	fi
	grep -q "start lerwatt-sim" "$WORK/err" || fail "unhelpful error: $(cat "$WORK/err")"
	pass "no hardware and no simulator -> clear error"
fi

# 1. Start a hot, fanless simulated board under full load.
"$SIM" --root "$ROOT" --load pattern:full --cooling bare --initial-temp 79 \
	--duration 60 --log "$WORK/truth.jsonl" 2>"$WORK/sim.err" &
SIM_PID=$!
for _ in $(seq 1 50); do [ -f "$ROOT/.lerwatt-sim" ] && break; sleep 0.1; done
[ -f "$ROOT/.lerwatt-sim" ] || fail "simulator did not come up: $(cat "$WORK/sim.err")"
ORIG=$(cat "$MAXF")
pass "simulator up, policy4 scaling_max_freq = $ORIG"

# A second simulator must not reset the live board, even with --force.
for extra in "" --force; do
	if "$SIM" --root "$ROOT" ${extra:+"$extra"} --load pattern:idle --virtual \
		--duration 0.1 2>"$WORK/second.err"; then
		fail "second simulator took over a live root ($extra)"
	fi
	grep -q "cannot lock simulator root" "$WORK/second.err" || fail "second simulator failed for an unexpected reason"
done
pass "live root rejects a second simulator, including --force"

# 2. lerwattd finds the simulator without being told where it is.
LERWATT_SIM_ROOT=$ROOT "$D" "${SYSROOT_ARGS[@]}" --duration 12 \
	--telemetry "$WORK/tel.jsonl" 2>"$WORK/d.err" || fail "lerwattd exited non-zero: $(cat "$WORK/d.err")"
grep -q "mode=simulation" "$WORK/d.err" || fail "did not detect the simulator: $(head -3 "$WORK/d.err")"
pass "auto-detected the simulator"
grep -q '"mode":"sim"' "$WORK/tel.jsonl" || fail "telemetry missing"
MIN_CEIL=$(grep -o '"id":1,[^}]*"ceil_mhz":[0-9]*' "$WORK/tel.jsonl" | sed 's/.*"ceil_mhz"://' | sort -n | head -1)
[ "$MIN_CEIL" -lt 2400 ] || fail "lerwattd never lowered the big0 ceiling (min seen $MIN_CEIL MHz)"
pass "lerwattd capped big0 down to $MIN_CEIL MHz while the board ran hot"
[ "$(cat "$MAXF")" = "$ORIG" ] || fail "scaling_max_freq not restored: $(cat "$MAXF") != $ORIG"
[ ! -f "$JOURNAL" ] || fail "journal left behind after a clean exit"
pass "clean exit restored every knob and removed the journal"

# 3. Crash recovery: kill -9 leaves the journal; --restore puts values back.
LERWATT_SIM_ROOT=$ROOT "$D" "${SYSROOT_ARGS[@]}" 2>"$WORK/d2.err" &
DPID=$!
for _ in $(seq 1 100); do
	[ "$(cat "$MAXF")" != "$ORIG" ] && break
	sleep 0.1
done
kill -9 "$DPID"
wait "$DPID" 2>/dev/null || true
[ "$(cat "$MAXF")" != "$ORIG" ] || fail "no cap was written before the kill"
[ -f "$JOURNAL" ] || fail "journal missing after kill -9"
LERWATT_SIM_ROOT=$ROOT "$D" "${SYSROOT_ARGS[@]}" --restore 2>/dev/null || fail "--restore failed"
[ "$(cat "$MAXF")" = "$ORIG" ] || fail "--restore left $(cat "$MAXF")"
pass "kill -9 then --restore recovered scaling_max_freq = $ORIG"

kill "$SIM_PID"
wait "$SIM_PID" 2>/dev/null || true
SIM_PID=
[ ! -f "$ROOT/.lerwatt-sim" ] || fail "simulator marker left behind"
pass "simulator removed its marker on exit"

# 4. Normal exits retain the root identity, not the live marker. Reuse the
# exact tree without --force after SIGTERM, duration expiry and SIGINT.
[ -f "$ROOT/.lerwatt-sim-root" ] || fail "persistent root identity missing"
if [ ${#SYSROOT_ARGS[@]} -eq 0 ]; then
	if LERWATT_SIM_ROOT=$ROOT "$D" --duration 0.1 2>"$WORK/stopped.err"; then
		fail "lerwattd attached to a stopped simulator"
	fi
fi
# Retain unrelated files and recovery data: restarting is not recursive cleanup.
printf 'preserve me\n' >"$ROOT/keep.txt"
mkdir -p "$(dirname "$JOURNAL")"
printf 'recovery sentinel\n' >"$JOURNAL"
for _ in 1 2; do
	"$SIM" --root "$ROOT" --load pattern:idle --virtual --duration 0.1 \
		2>"$WORK/restart.err" || fail "same-root restart failed: $(cat "$WORK/restart.err")"
	[ ! -f "$ROOT/.lerwatt-sim" ] || fail "duration exit left a live marker"
done
"$SIM" --root "$ROOT" --load pattern:idle 2>"$WORK/restart.err" &
SIM_PID=$!
for _ in $(seq 1 50); do [ -f "$ROOT/.lerwatt-sim" ] && break; sleep 0.1; done
[ -f "$ROOT/.lerwatt-sim" ] || fail "restart before SIGINT failed"
kill -INT "$SIM_PID"
wait "$SIM_PID" || fail "SIGINT exit failed"
SIM_PID=
[ ! -f "$ROOT/.lerwatt-sim" ] || fail "SIGINT exit left a live marker"
[ "$(cat "$ROOT/keep.txt")" = 'preserve me' ] || fail "restart changed unrelated data"
[ "$(cat "$JOURNAL")" = 'recovery sentinel' ] || fail "restart changed the recovery journal"
pass "same root restarts after SIGTERM, duration expiry and SIGINT; data preserved"

# 5. Unidentified non-empty roots (including pre-fix leftovers) stay protected.
mkdir -p "$WORK/foreign"
printf 'not a simulator\n' >"$WORK/foreign/keep.txt"
if "$SIM" --root "$WORK/foreign" --load pattern:idle --virtual --duration 0.1 \
	2>"$WORK/foreign.err"; then
	fail "simulator accepted an unidentified non-empty root"
fi
grep -q "not empty and has no simulator marker" "$WORK/foreign.err" || fail "unexpected foreign-root error"
[ "$(cat "$WORK/foreign/keep.txt")" = 'not a simulator' ] || fail "foreign data changed"
pass "unidentified non-empty roots remain protected"
echo "e2e: all checks passed"
