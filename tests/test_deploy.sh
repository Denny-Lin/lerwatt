#!/bin/sh
# SPDX-License-Identifier: MIT
# Exercise deployment without sudo, host installation or kernel writes.
set -eu

repo=$(CDPATH= cd -P "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/lerwatt-deploy-test.XXXXXX")
tmp=$(CDPATH= cd -P "$tmp" && pwd)
trap 'rm -rf "$tmp"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
fixture=$tmp/source\ tree
tools=$tmp/tools
mkdir -p "$fixture/src" "$tools"
cp "$repo/deploy.sh" "$fixture/deploy.sh"
chmod 755 "$fixture/deploy.sh"
host=$tmp/host
mkdir -p "$host/etc/systemd/system" "$host/run/systemd/system"
# Redirect only this fixture's literal host paths; production has no test-root
# environment override. No test ever writes /etc, /run or /var/lib on the host.
python3 - "$fixture/deploy.sh" "$host" <<'PY_PATHS'
from pathlib import Path
import sys
p=Path(sys.argv[1]); host=sys.argv[2]
s=p.read_text()
for path in ('/etc/systemd/system/lerwatt.service','/etc/lerwatt','/var/lib/lerwatt','/run/systemd/system'):
    s=s.replace(path,host+path)
p.write_text(s)
PY_PATHS
SERVICE_UNIT_PATH=$host/etc/systemd/system/lerwatt.service
SERVICE_CONFIG=$host/etc/lerwatt/lerwatt.conf
SERVICE_ACTIVE=$tmp/service.active
SERVICE_ENABLED=$tmp/service.enabled
export SERVICE_UNIT_PATH SERVICE_CONFIG SERVICE_ACTIVE SERVICE_ENABLED
: > "$fixture/Makefile"
: > "$fixture/src/lerwattd.c"
TEST_LOG=$tmp/events
STUB_TEMPLATE=$tmp/daemon-template
CHILD_PID=$tmp/child.pid
export TEST_LOG STUB_TEMPLATE CHILD_PID
PATH=$tools:$PATH
export PATH
unset PREFIX DESTDIR

cat > "$tools/id" <<'EOF'
#!/bin/sh
printf '%s\n' "${FAKE_UID:-1000}"
EOF
cat > "$tools/uname" <<'EOF'
#!/bin/sh
printf '%s\n' Linux
EOF
cat > "$tools/stat" <<'EOF'
#!/bin/sh
[ "$1" = -c ] || exit 1
if [ "$2" = '%h' ]; then
    if [ "$3" = "${HARDLINK_PATH:-}" ]; then printf '2\n'; else printf '1\n'; fi
    exit 0
fi
[ "$2" = '%u %a' ] || exit 1
if [ "$3" = "${UNTRUSTED_PATH:-}" ]; then
    printf '%s\n' "${UNTRUSTED_STAT:-1000 755}"
else
    printf '%s\n' '0 755'
fi
EOF
cat > "$tools/systemctl" <<'EOF'
#!/bin/sh
printf 'SYSTEMCTL' >> "$TEST_LOG"
for arg do printf ' %s' "$arg" >> "$TEST_LOG"; done
printf '\n' >> "$TEST_LOG"
case $1 in
    list-units) exit "${SYSTEMD_EXIT:-0}" ;;
    show)
        case $4 in
            FragmentPath) if [ -n "${FOREIGN_FRAGMENT:-}" ]; then printf '%s\n' "$FOREIGN_FRAGMENT"; elif [ -f "$SERVICE_UNIT_PATH" ]; then printf '%s\n' "$SERVICE_UNIT_PATH"; fi ;;
            DropInPaths) printf '%s\n' "${SERVICE_DROPINS:-}" ;;
        esac ;;
    is-active)
        [ "$3" = "${CONFLICT_SERVICE:-}" ] && exit 0
        if [ "$3" = lerwatt.service ] && [ -e "$SERVICE_ACTIVE" ]; then exit 0; fi
        exit 3 ;;
    stop)
        [ "${STOP_EXIT:-0}" = 0 ] || exit "$STOP_EXIT"
        rm -f "$SERVICE_ACTIVE" ;;
    disable) rm -f "$SERVICE_ENABLED" ;;
    enable)
        begins=$(grep -c '^MAINTENANCE BEGIN$' "$TEST_LOG" || :)
        ends=$(grep -c '^MAINTENANCE END$' "$TEST_LOG" || :)
        [ "$begins" = "$ends" ] || exit 98
        [ "${ENABLE_EXIT:-0}" = 0 ] || exit "$ENABLE_EXIT"
        : > "$SERVICE_ENABLED"; : > "$SERVICE_ACTIVE" ;;
    start|restart) : > "$SERVICE_ACTIVE" ;;
    is-enabled) [ -e "$SERVICE_ENABLED" ] ;;
    status) [ -e "$SERVICE_ACTIVE" ] || exit 3 ;;
    daemon-reload) ;;
    *) exit 9 ;;
esac
EOF
cat > "$tools/journalctl" <<'EOF'
#!/bin/sh
printf 'JOURNALCTL\n' >> "$TEST_LOG"
EOF
cat > "$tools/sudo" <<'EOF'
#!/bin/sh
printf 'SUDO\n' >> "$TEST_LOG"
exec env FAKE_UID=0 "$@"
EOF
cat > "$tools/make" <<'EOF'
#!/bin/sh
printf 'BUILD\n' >> "$TEST_LOG"
[ "${FAIL_BUILD:-0}" = 0 ] || exit 11
[ "$1" = -C ] || exit 12
case $3 in
    build/lerwattd-standalone) feature=none ;;
    build/lerwattd) feature=bpf ;;
    *) exit 12 ;;
esac
printf 'BUILD_TARGET %s\n' "$3" >> "$TEST_LOG"
mkdir -p "$2/build"
sed "s/^built_contract=none\$/built_contract=$feature/" "$STUB_TEMPLATE" > "$2/$3"
chmod 755 "$2/$3"
EOF
cat > "$STUB_TEMPLATE" <<'EOF'
#!/bin/sh
set -eu
built_contract=none
mode=run
for arg do
    case $arg in
        --check) mode=check ;;
        --restore) mode=restore ;;
        --dry-run) mode=preview ;;
        --help) mode=help ;;
        --build-features) mode=features ;;
    esac
done
printf 'MODE %s\n' "$mode" >> "$TEST_LOG"
for arg do printf 'ARG %s\n' "$arg" >> "$TEST_LOG"; done
case $mode in
    features) printf 'contract=%s\n' "${FORCE_FEATURES:-$built_contract}"; exit 0 ;;
    check) exit "${CHECK_EXIT:-0}" ;;
    restore)
        [ "${RESTORE_EXIT:-0}" = 0 ] || exit "$RESTORE_EXIT"
        while [ "$#" -gt 0 ]; do
            if [ "$1" = -- ]; then
                shift
                printf 'MAINTENANCE BEGIN\n' >> "$TEST_LOG"
                if "$@"; then result=0; else result=$?; fi
                printf 'MAINTENANCE END\n' >> "$TEST_LOG"
                exit "$result"
            fi
            shift
        done
        exit 0 ;;
    preview) exit 0 ;;
    help) exit "${HELP_EXIT:-0}" ;;
esac
case ${RUN_ACTION:-exit} in
    ignore-int)
        # Model startup before the C daemon replaces inherited SIGINT ignore.
        trap '' INT
        trap 'printf "CHILD TERM\n" >> "$TEST_LOG"; exit 0' TERM
        printf '%s\n' "$$" > "$CHILD_PID"
        while :; do sleep 1; done ;;
    wait)
        trap 'printf "CHILD TERM\n" >> "$TEST_LOG"; exit 0' TERM
        trap 'printf "CHILD HUP\n" >> "$TEST_LOG"; exit 0' HUP
        trap 'printf "CHILD INT\n" >> "$TEST_LOG"; exit 0' INT
        printf '%s\n' "$$" > "$CHILD_PID"
        while :; do sleep 1; done ;;
    kill) kill -KILL "$$" ;;
esac
exit "${RUN_EXIT:-0}"
EOF
chmod 755 "$tools/id" "$tools/uname" "$tools/stat" "$tools/systemctl" "$tools/journalctl" "$tools/sudo" "$tools/make" "$STUB_TEMPLATE"

die()
{
    printf 'test_deploy: %s\n' "$*" >&2
    cat "$TEST_LOG" >&2
    exit 1
}
has()
{
    grep -F -x "$1" "$TEST_LOG" >/dev/null || die "missing event: $1"
}
lacks()
{
    if grep -F -x "$1" "$TEST_LOG" >/dev/null; then die "unexpected event: $1"; fi
}
reset_log()
{
    : > "$TEST_LOG"
}
expect_status()
{
    expected=$1
    shift
    if "$@" > "$tmp/output" 2>&1; then actual=0; else actual=$?; fi
    [ "$actual" = "$expected" ] || {
        cat "$tmp/output" >&2
        die "expected status $expected, got $actual"
    }
}

# Staging may build but must not invoke a daemon, sudo or host restoration.
reset_log
PREFIX=/opt/lerwatt DESTDIR="$tmp/stage area" "$fixture/deploy.sh" install-tools > "$tmp/output"
stage=$tmp/stage\ area/opt/lerwatt
[ -x "$stage/bin/lerwatt" ] && [ -x "$stage/sbin/lerwattd" ] || die 'staged installation missing files'
has BUILD
lacks SUDO
lacks 'MODE restore'
lacks 'MODE help'

# The installed helper locates its daemon relative to itself, including spaces.
reset_log
FAKE_UID=0 "$stage/bin/lerwatt" check > "$tmp/output" 2>&1
has 'MODE check'
has 'ARG --sysroot'
has 'ARG /'
has 'ARG --no-contract'
lacks BUILD
lacks 'MODE restore'

# Preview is non-actuating even if a prior journal could need restoration.
reset_log
"$fixture/deploy.sh" preview --duration=180 --telemetry "$tmp/forecast data.jsonl" > "$tmp/output" 2>&1
has BUILD
has SUDO
has 'MODE preview'
has 'ARG --dry-run'
has 'ARG --no-contract'
has "ARG $tmp/forecast data.jsonl"
lacks 'MODE restore'

# A failed check prevents starting an active child or performing restoration.
reset_log
expect_status 1 env CHECK_EXIT=4 "$fixture/deploy.sh" run
has 'MODE check'
lacks 'MODE run'
lacks 'MODE restore'

# Forward valid options, force the hardware root, then recover after the child.
reset_log
"$fixture/deploy.sh" run --duration 17 --policy rule --period-ms 50 \
    --soft-limit 72.5 --temperature-sensor /sys/class/thermal/thermal_zone0/temp \
    --telemetry "$tmp/hardware data.jsonl" > "$tmp/output" 2>&1
has 'MODE run'
has 'MODE restore'
has 'ARG --duration'
has 'ARG 17'
has 'ARG 300'
has 'ARG rule'
has 'ARG /sys/class/thermal/thermal_zone0/temp'
run_line=$(grep -n '^MODE run$' "$TEST_LOG" | cut -d : -f 1)
restore_line=$(grep -n '^MODE restore$' "$TEST_LOG" | cut -d : -f 1)
[ "$restore_line" -gt "$run_line" ] || die 'restore occurred before the active child'

# Invalid/root-overriding options are rejected before building or sudo.
for option in --sysroot --no-contract --cross-cluster; do
    reset_log
    expect_status 1 "$fixture/deploy.sh" run "$option" /
    lacks BUILD
    lacks SUDO
done
reset_log
expect_status 1 "$fixture/deploy.sh" run --duration 0
lacks BUILD
expect_status 1 "$fixture/deploy.sh" run --period-ms 9
expect_status 1 "$fixture/deploy.sh" run --soft-limit 0.0
for root_path in / // ///; do
    reset_log
    expect_status 1 env PREFIX="$root_path" "$fixture/deploy.sh" install-tools
    expect_status 1 env DESTDIR="$root_path" "$fixture/deploy.sh" install-tools
    lacks BUILD
    lacks SUDO
done

# Preserve nonzero child status, but surface a separate restore failure as 2.
reset_log
expect_status 7 env RUN_EXIT=7 "$fixture/deploy.sh" run
has 'MODE restore'
reset_log
expect_status 2 env RESTORE_EXIT=23 "$fixture/deploy.sh" run
has 'MODE restore'
reset_log
expect_status 137 env RUN_ACTION=kill "$fixture/deploy.sh" run
has 'MODE restore'

# TERM reaches the owned child; it exits before the supervisor restores.
reset_log
rm -f "$CHILD_PID"
RUN_ACTION=wait "$fixture/deploy.sh" run > "$tmp/output" 2>&1 &
supervisor=$!
attempt=0
while [ ! -s "$CHILD_PID" ]; do
    attempt=$((attempt + 1))
    [ "$attempt" -lt 100 ] || { kill -TERM "$supervisor" 2>/dev/null || :; die 'trial child did not start'; }
    sleep 0.02
done
kill -TERM "$supervisor"
if wait "$supervisor"; then actual=0; else actual=$?; fi
[ "$actual" = 143 ] || die "signal exit was $actual instead of 143"
has 'CHILD TERM'
has 'MODE restore'
child_line=$(grep -n '^CHILD TERM$' "$TEST_LOG" | cut -d : -f 1)
restore_line=$(grep -n '^MODE restore$' "$TEST_LOG" | cut -d : -f 1)
[ "$restore_line" -gt "$child_line" ] || die 'signal recovery ran before child termination'

# Keep the supervisor in the foreground so it can receive INT normally, while
# its daemon models the inherited SIGINT-ignore window before handler setup.
reset_log
rm -f "$CHILD_PID"
WRAPPER_PID=$tmp/wrapper.pid
INT_DONE=$tmp/int.done
export WRAPPER_PID
(
    attempt=0
    while [ ! -s "$CHILD_PID" ] || [ ! -s "$WRAPPER_PID" ]; do
        attempt=$((attempt + 1))
        [ "$attempt" -lt 100 ] || exit 1
        sleep 0.02
    done
    kill -INT "$(cat "$WRAPPER_PID")"
    sleep 2
    if [ ! -e "$INT_DONE" ]; then
        printf 'INT TIMEOUT\n' >> "$TEST_LOG"
        kill -TERM "$(cat "$CHILD_PID")" 2>/dev/null || :
        kill -TERM "$(cat "$WRAPPER_PID")" 2>/dev/null || :
    fi
) &
signaler=$!
if FAKE_UID=0 RUN_ACTION=ignore-int sh -c 'printf "%s\n" "$$" > "$WRAPPER_PID"; exec "$1" run' sh "$fixture/deploy.sh" > "$tmp/output" 2>&1; then
    actual=0
else
    actual=$?
fi
: > "$INT_DONE"
wait "$signaler" || die 'INT signal sender failed'
[ "$actual" = 130 ] || die "INT exit was $actual instead of 130"
lacks 'INT TIMEOUT'
has 'CHILD TERM'
has 'MODE restore'
child_line=$(grep -n '^CHILD TERM$' "$TEST_LOG" | cut -d : -f 1)
restore_line=$(grep -n '^MODE restore$' "$TEST_LOG" | cut -d : -f 1)
[ "$restore_line" -gt "$child_line" ] || die 'INT recovery ran before child termination'

# A normal install is still confined to a test prefix, using fake sudo.
reset_log
PREFIX="$tmp/live prefix" "$fixture/deploy.sh" install-tools > "$tmp/output" 2>&1
has SUDO
has 'MAINTENANCE BEGIN'
has 'MAINTENANCE END'
has 'ARG install-tools-locked'
[ -x "$tmp/live prefix/bin/lerwatt" ] || die 'installed helper missing'
printf '\n# installed generation\n' >> "$tmp/live prefix/sbin/lerwattd"
cp "$tmp/live prefix/sbin/lerwattd" "$tmp/old-daemon"
reset_log
expect_status 1 env PREFIX="$tmp/live prefix" RESTORE_EXIT=23 "$fixture/deploy.sh" install-tools
has 'MODE restore'
cmp "$tmp/old-daemon" "$tmp/live prefix/sbin/lerwattd" >/dev/null || die 'failed restoration replaced installed daemon'
reset_log
expect_status 1 env PREFIX="$tmp/live prefix" RESTORE_EXIT=23 "$fixture/deploy.sh" uninstall
has 'MODE restore'
[ -x "$tmp/live prefix/bin/lerwatt" ] && [ -x "$tmp/live prefix/sbin/lerwattd" ] || die 'failed recovery removed installed files'
reset_log
FAKE_UID=0 "$tmp/live prefix/bin/lerwatt" uninstall > "$tmp/output" 2>&1
has 'MODE restore'
has 'ARG uninstall-locked'
has 'MAINTENANCE BEGIN'
has 'MAINTENANCE END'
[ ! -e "$tmp/live prefix/bin/lerwatt" ] && [ ! -e "$tmp/live prefix/sbin/lerwattd" ] || die 'uninstall left installed files'

# A source upgrade uses its freshly built supervisor rather than a legacy
# installed daemon that might silently ignore the lock-held command arguments.
reset_log
mkdir -p "$tmp/legacy/sbin"
printf '#!/bin/sh\nprintf "LEGACY EXECUTED\\n" >> "$TEST_LOG"\nexit 0\n' > "$tmp/legacy/sbin/lerwattd"
chmod 755 "$tmp/legacy/sbin/lerwattd"
PREFIX="$tmp/legacy" "$fixture/deploy.sh" install-tools > "$tmp/output" 2>&1
has 'MAINTENANCE BEGIN'
lacks 'LEGACY EXECUTED'
[ -x "$tmp/legacy/bin/lerwatt" ] || die 'legacy upgrade did not install the helper'

# Stage removal never runs the staged recovery executable.
reset_log
PREFIX=/opt/lerwatt DESTDIR="$tmp/stage area" "$fixture/deploy.sh" uninstall > "$tmp/output"
lacks SUDO
lacks 'MODE restore'
[ ! -e "$stage/bin/lerwatt" ] && [ ! -e "$stage/sbin/lerwattd" ] || die 'staged uninstall left files'

# A stale or incompatible binary fails before sudo or replacement.
reset_log
mkdir -p "$tmp/incompatible/sbin"
printf 'keep this binary\n' > "$tmp/incompatible/sbin/lerwattd"
expect_status 1 env PREFIX="$tmp/incompatible" HELP_EXIT=126 "$fixture/deploy.sh" install-tools
has 'MODE help'
lacks SUDO
lacks 'MODE restore'
grep -F -x 'keep this binary' "$tmp/incompatible/sbin/lerwattd" >/dev/null || die 'incompatible daemon replaced installed binary'

# A failed source build cannot replace an existing installation.
reset_log
mkdir -p "$tmp/keep/sbin"
printf 'keep this binary\n' > "$tmp/keep/sbin/lerwattd"
expect_status 1 env PREFIX="$tmp/keep" FAIL_BUILD=1 "$fixture/deploy.sh" install-tools
grep -F -x 'keep this binary' "$tmp/keep/sbin/lerwattd" >/dev/null || die 'build failure replaced installed binary'
lacks SUDO

# Root installation must reject untrusted ancestors and recovery executables.
for ownership in '1000 755' '0 775' '0 757'; do
    reset_log
    mkdir -p "$tmp/untrusted/sbin"
    cp "$STUB_TEMPLATE" "$tmp/untrusted/sbin/lerwattd"
    chmod 755 "$tmp/untrusted/sbin/lerwattd"
    expect_status 1 env FAKE_UID=0 PREFIX="$tmp/untrusted" \
        UNTRUSTED_PATH="$tmp/untrusted" UNTRUSTED_STAT="$ownership" "$fixture/deploy.sh" install-tools
    lacks 'MODE restore'
    [ ! -e "$tmp/untrusted/bin/lerwatt" ] || die 'untrusted prefix was installed'
done
reset_log
expect_status 1 env FAKE_UID=0 PREFIX="$tmp/untrusted" \
    UNTRUSTED_PATH="$tmp/untrusted/sbin/lerwattd" "$fixture/deploy.sh" uninstall
lacks 'MODE restore'
[ -x "$tmp/untrusted/sbin/lerwattd" ] || die 'untrusted recovery executable was removed'

# Symlinked ancestors are rejected even when their final files are regular.
reset_log
ln -s "$tmp/untrusted" "$tmp/prefix-link"
expect_status 1 env FAKE_UID=0 PREFIX="$tmp/prefix-link" "$fixture/deploy.sh" install-tools
lacks 'MODE restore'
reset_log
mkdir -p "$tmp/stage-link/opt"
ln -s "$tmp/untrusted" "$tmp/stage-link/opt/lerwatt"
expect_status 1 env FAKE_UID=0 PREFIX=/opt/lerwatt DESTDIR="$tmp/stage-link" "$fixture/deploy.sh" install-tools
lacks 'MODE help'
lacks 'MODE restore'
reset_log
expect_status 1 env FAKE_UID=0 PREFIX=/opt/lerwatt DESTDIR="$tmp/root-stage" \
    UNTRUSTED_PATH="$tmp" UNTRUSTED_STAT='0 1777' "$fixture/deploy.sh" install-tools
lacks 'MODE help'
lacks SUDO
[ ! -e "$tmp/root-stage" ] || die 'root staging wrote under untrusted ancestor'
reset_log
expect_status 1 "$fixture/deploy.sh" install-locked
expect_status 1 env DESTDIR="$tmp/stage" FAKE_UID=0 "$fixture/deploy.sh" uninstall-locked
lacks 'MODE restore'
lacks SUDO

# Persistent installation validates hardware, writes only an owned service,
# exits maintenance before activation and starts/retains boot enablement.
reset_log
service_prefix=$tmp/persistent\ prefix
PREFIX="$service_prefix" "$fixture/deploy.sh" install --policy rule --period-ms 250 \
    --soft-limit 72.5 --temperature-sensor /sys/class/thermal/thermal_zone0/temp > "$tmp/output" 2>&1
has 'MODE check'
has 'ARG install-locked'
has 'SYSTEMCTL daemon-reload'
has 'SYSTEMCTL enable --now lerwatt.service'
[ -f "$SERVICE_UNIT_PATH" ] && [ -f "$SERVICE_CONFIG" ] || die 'persistent files missing'
[ -f "$SERVICE_ACTIVE" ] && [ -f "$SERVICE_ENABLED" ] || die 'service was not activated/enabled'
grep -F -x 'policy=rule' "$SERVICE_CONFIG" >/dev/null || die 'policy override was not persisted'
grep -F -x 'period_ms=250' "$SERVICE_CONFIG" >/dev/null || die 'period override was not persisted'
grep -F -x 'soft_limit_c=72.5' "$SERVICE_CONFIG" >/dev/null || die 'soft limit was not persisted'
grep -F -x 'contract=off' "$SERVICE_CONFIG" >/dev/null || die 'new installation unexpectedly enabled coordination'
grep -F -x 'Type=notify' "$SERVICE_UNIT_PATH" >/dev/null || die 'notify service type missing'
grep -F -x 'WatchdogSec=30s' "$SERVICE_UNIT_PATH" >/dev/null || die 'independent watchdog missing'
grep -F -x 'TimeoutAbortSec=15s' "$SERVICE_UNIT_PATH" >/dev/null || die 'watchdog abort timeout is unbounded'
grep -F -x 'Restart=always' "$SERVICE_UNIT_PATH" >/dev/null || die 'restart policy missing'
grep -F -x 'StateDirectory=lerwatt' "$SERVICE_UNIT_PATH" >/dev/null || die 'persistent state directory missing'
grep -F -x "ExecStart=\"$service_prefix/bin/lerwatt\" service-run" "$SERVICE_UNIT_PATH" >/dev/null || die 'custom-prefix unit quoting is wrong'
python3 - "$SERVICE_CONFIG" <<'PY_MODE'
from pathlib import Path
import sys
assert Path(sys.argv[1]).stat().st_mode & 0o777 == 0o600
PY_MODE

# The service main operation assembles fixed hardware arguments and execs the
# worker without a finite duration, model reset or unbounded telemetry file.
reset_log
FAKE_UID=0 PREFIX="$service_prefix" "$service_prefix/bin/lerwatt" service-run > "$tmp/output" 2>&1
has 'MODE run'; has 'ARG --watchdog'; has 'ARG --model-state'; has 'ARG --duration'; has 'ARG 0'
has "ARG $host/var/lib/lerwatt/model.state"
has 'ARG rule'; has 'ARG 250'; has 'ARG 72.5'
has 'ARG --no-contract'
lacks 'ARG --telemetry'; lacks 'MODE restore'
reset_log
FAKE_UID=0 PREFIX="$service_prefix" "$service_prefix/bin/lerwatt" service-check > "$tmp/output" 2>&1
has 'MODE check'; lacks 'ARG --watchdog'; lacks 'ARG --model-state'
reset_log
FAKE_UID=0 PREFIX="$service_prefix" "$service_prefix/bin/lerwatt" service-restore > "$tmp/output" 2>&1
has 'MODE restore'

# Optional coordination chooses a BPF-enabled build before replacing the
# service, persists across a plain upgrade, and can be explicitly disabled.
reset_log
expect_status 1 env PREFIX="$service_prefix" "$fixture/deploy.sh" install --lertick
lacks 'SYSTEMCTL stop lerwatt.service'; lacks 'MAINTENANCE BEGIN'
reset_log
PREFIX="$service_prefix" "$fixture/deploy.sh" install --lertick --period-ms 100 > "$tmp/output" 2>&1
has 'BUILD_TARGET build/lerwattd'
grep -F -x 'contract=auto' "$SERVICE_CONFIG" >/dev/null || die 'coordination opt-in was not persisted'
reset_log
FAKE_UID=0 "$service_prefix/bin/lerwatt" service-run > "$tmp/output" 2>&1
has 'MODE features'; has 'MODE run'; lacks 'ARG --no-contract'
reset_log
PREFIX="$service_prefix" "$fixture/deploy.sh" install > "$tmp/output" 2>&1
has 'BUILD_TARGET build/lerwattd'
grep -F -x 'contract=auto' "$SERVICE_CONFIG" >/dev/null || die 'plain upgrade disabled coordination'
reset_log
FAKE_UID=0 "$service_prefix/bin/lerwatt" status > "$tmp/output" 2>&1
grep -F 'Lertick coordination configured: auto' "$tmp/output" >/dev/null || die 'status omitted configured coordination mode'
reset_log
expect_status 1 env PREFIX="$service_prefix" FORCE_FEATURES=none "$fixture/deploy.sh" install --lertick
lacks 'SYSTEMCTL stop lerwatt.service'; lacks 'MAINTENANCE BEGIN'
reset_log
expect_status 1 env PREFIX="$service_prefix" FAIL_BUILD=1 "$fixture/deploy.sh" install --lertick
lacks 'SYSTEMCTL stop lerwatt.service'; lacks 'MAINTENANCE BEGIN'
grep -F 'BPF-enabled build failed' "$tmp/output" >/dev/null || die 'BPF build failure lacks actionable diagnostic'
reset_log
PREFIX="$service_prefix" "$fixture/deploy.sh" install --no-lertick > "$tmp/output" 2>&1
has 'BUILD_TARGET build/lerwattd-standalone'
grep -F -x 'contract=off' "$SERVICE_CONFIG" >/dev/null || die 'coordination opt-out was not persisted'
printf 'contract=auto\n' >> "$SERVICE_CONFIG"
reset_log
expect_status 1 env FAKE_UID=0 "$service_prefix/bin/lerwatt" service-run
lacks 'MODE run'
sed '$d' "$SERVICE_CONFIG" > "$tmp/config.clean"
cp "$tmp/config.clean" "$SERVICE_CONFIG"
cp "$SERVICE_CONFIG" "$tmp/contract-off.saved"
printf 'contract=auto\nperiod_ms=250\n' >> "$SERVICE_CONFIG"
reset_log
expect_status 1 env FAKE_UID=0 "$service_prefix/bin/lerwatt" service-run
lacks 'MODE run'
reset_log
expect_status 1 env PREFIX="$service_prefix" "$fixture/deploy.sh" install
lacks BUILD; lacks 'SYSTEMCTL stop lerwatt.service'; lacks 'MAINTENANCE BEGIN'
cp "$tmp/contract-off.saved" "$SERVICE_CONFIG"
reset_log
expect_status 1 "$fixture/deploy.sh" install --lertick --no-lertick
lacks BUILD; lacks SUDO
reset_log
expect_status 1 "$fixture/deploy.sh" run --lertick
lacks BUILD; lacks SUDO

# Staging the optional build still never runs a daemon or sudo.
reset_log
PREFIX=/opt/lerwatt DESTDIR="$tmp/contract-stage" "$fixture/deploy.sh" install --lertick > "$tmp/output" 2>&1
has 'BUILD_TARGET build/lerwattd'
lacks SUDO; lacks 'MODE features'; lacks 'MODE help'; lacks 'MODE restore'

# Never automatically disable a competing policy service or replace a foreign
# unit/drop-in. Each refusal occurs before stopping the owned service.
cp "$SERVICE_UNIT_PATH" "$tmp/unit.saved"
cp "$SERVICE_CONFIG" "$tmp/config.saved"
reset_log
expect_status 1 env PREFIX="$service_prefix" CONFLICT_SERVICE=power-profiles-daemon.service "$fixture/deploy.sh" install
lacks 'SYSTEMCTL stop lerwatt.service'; lacks 'MAINTENANCE BEGIN'
cmp "$tmp/unit.saved" "$SERVICE_UNIT_PATH" >/dev/null || die 'conflict changed service'
cmp "$tmp/config.saved" "$SERVICE_CONFIG" >/dev/null || die 'conflict changed config'
reset_log
expect_status 1 env PREFIX="$service_prefix" FOREIGN_FRAGMENT=/usr/lib/systemd/system/foreign.service "$fixture/deploy.sh" install
lacks 'SYSTEMCTL stop lerwatt.service'
reset_log
expect_status 1 env PREFIX="$service_prefix" SERVICE_DROPINS=/etc/systemd/system/lerwatt.service.d/foreign.conf "$fixture/deploy.sh" install
lacks 'SYSTEMCTL stop lerwatt.service'
printf '# Foreign service\n' > "$SERVICE_UNIT_PATH"
reset_log
expect_status 1 env PREFIX="$service_prefix" "$fixture/deploy.sh" install
lacks 'SYSTEMCTL stop lerwatt.service'
cp "$tmp/unit.saved" "$SERVICE_UNIT_PATH"

# Unit/config symlinks cannot redirect privileged service file operations.
rm -f "$SERVICE_UNIT_PATH"
ln -s "$tmp/unit.saved" "$SERVICE_UNIT_PATH"
reset_log
expect_status 1 env PREFIX="$service_prefix" "$fixture/deploy.sh" install
lacks 'SYSTEMCTL stop lerwatt.service'
rm -f "$SERVICE_UNIT_PATH"
cp "$tmp/unit.saved" "$SERVICE_UNIT_PATH"
rm -f "$SERVICE_CONFIG"
ln -s "$tmp/config.saved" "$SERVICE_CONFIG"
reset_log
expect_status 1 env FAKE_UID=0 PREFIX="$service_prefix" "$service_prefix/bin/lerwatt" service-run
lacks 'MODE run'
rm -f "$SERVICE_CONFIG"
cp "$tmp/config.saved" "$SERVICE_CONFIG"

# A supported unit is not installed when hardware checks or systemd fail.
reset_log
expect_status 1 env PREFIX="$service_prefix" CHECK_EXIT=4 "$fixture/deploy.sh" install
lacks 'SYSTEMCTL stop lerwatt.service'; lacks 'MAINTENANCE BEGIN'
reset_log
expect_status 1 env PREFIX="$service_prefix" SYSTEMD_EXIT=1 "$fixture/deploy.sh" install
lacks 'MODE check'; lacks 'MAINTENANCE BEGIN'

# Non-systemd users retain a genuine tools-only install, and staged default
# installation never invokes any host service operation.
reset_log
PREFIX="$tmp/non-systemd-tools" SYSTEMD_EXIT=1 "$fixture/deploy.sh" install-tools > "$tmp/output" 2>&1
lacks 'SYSTEMCTL list-units --no-pager --no-legend'
lacks 'SYSTEMCTL enable --now lerwatt.service'
reset_log
PREFIX=/opt/lerwatt DESTDIR="$tmp/persistent-stage" SYSTEMD_EXIT=1 "$fixture/deploy.sh" install > "$tmp/output" 2>&1
lacks SUDO
lacks 'MODE help'
lacks 'MODE restore'
lacks 'SYSTEMCTL enable --now lerwatt.service'
[ -x "$tmp/persistent-stage/opt/lerwatt/bin/lerwatt" ] || die 'staged default install missing tools'

# Config is parsed as data, never sourced as a root shell script.
printf 'policy=$(touch %s)\n' "$tmp/injected" > "$SERVICE_CONFIG"
reset_log
expect_status 1 env FAKE_UID=0 PREFIX="$service_prefix" "$service_prefix/bin/lerwatt" service-run
[ ! -e "$tmp/injected" ] || die 'configuration was executed as shell code'
lacks 'MODE run'
cp "$tmp/config.saved" "$SERVICE_CONFIG"
reset_log
expect_status 1 env FAKE_UID=0 PREFIX="$service_prefix" HARDLINK_PATH="$SERVICE_CONFIG" "$service_prefix/bin/lerwatt" service-run
lacks 'MODE run'

# Stop/disable happens before taking the restore lock. If restoration fails,
# the stopped service, recovery data and binaries remain available for repair.
reset_log
expect_status 1 env PREFIX="$service_prefix" RESTORE_EXIT=23 "$fixture/deploy.sh" uninstall
has 'SYSTEMCTL stop lerwatt.service'; has 'SYSTEMCTL disable lerwatt.service'
[ ! -e "$SERVICE_ACTIVE" ] && [ ! -e "$SERVICE_ENABLED" ] || die 'failed uninstall left the service enabled'
[ -x "$service_prefix/bin/lerwatt" ] && [ -x "$service_prefix/sbin/lerwattd" ] && [ -f "$SERVICE_UNIT_PATH" ] || die 'failed restoration removed recovery tools'
grep -F 'stopped/disabled' "$tmp/output" >/dev/null || die 'failure did not explain stopped/disabled state'
reset_log
FAKE_UID=0 "$service_prefix/bin/lerwatt" uninstall > "$tmp/output" 2>&1
has 'MODE restore'; has 'ARG uninstall-locked'; has 'SYSTEMCTL daemon-reload'
[ ! -e "$service_prefix/bin/lerwatt" ] && [ ! -e "$SERVICE_UNIT_PATH" ] || die 'persistent uninstall left managed files'
[ -f "$SERVICE_CONFIG" ] || die 'uninstall removed user configuration'

# Spaces, dollar signs and percent specifiers in a protected custom prefix
# remain literal systemd path characters, never unit syntax.
reset_log
special_prefix=$tmp/'prefix % $ value'
PREFIX="$special_prefix" "$fixture/deploy.sh" install > "$tmp/output" 2>&1
python3 - "$SERVICE_UNIT_PATH" "$special_prefix" <<'PY_ESCAPE'
from pathlib import Path
import sys
escaped=(sys.argv[2]+'/bin/lerwatt').replace('\\','\\\\').replace('"','\\"').replace('%','%%')
line='ExecStart="'+escaped+'" service-run'
assert line in Path(sys.argv[1]).read_text().splitlines(),line
PY_ESCAPE
FAKE_UID=0 "$special_prefix/bin/lerwatt" uninstall > "$tmp/output" 2>&1
[ ! -e "$SERVICE_UNIT_PATH" ] || die 'special-prefix uninstall left unit'

# systemd refuses quotes/backslashes in executable names. Reject persistent
# installs before a build, privilege escalation, stop or unit-file change.
for rejected_prefix in "$tmp/quote'path" "$tmp/quote\"path" "$tmp/back\\slash"; do
    reset_log
    expect_status 1 env PREFIX="$rejected_prefix" "$fixture/deploy.sh" install
    lacks BUILD; lacks SUDO; lacks 'SYSTEMCTL stop lerwatt.service'
    [ ! -e "$SERVICE_UNIT_PATH" ] || die 'unsupported prefix wrote a unit'
    reset_log
    expect_status 1 env FAKE_UID=0 PREFIX="$rejected_prefix" "$fixture/deploy.sh" install-locked
    lacks BUILD; lacks 'MODE restore'
    [ ! -e "$SERVICE_UNIT_PATH" ] || die 'internal unsupported prefix wrote a unit'
    # The same characters remain valid data paths for tools/staging.
    reset_log
    PREFIX="$rejected_prefix" "$fixture/deploy.sh" install-tools > "$tmp/output" 2>&1
    lacks 'SYSTEMCTL enable --now lerwatt.service'
    [ -x "$rejected_prefix/bin/lerwatt" ] || die 'tools-only prefix was incorrectly rejected'
    reset_log
    PREFIX="$rejected_prefix" DESTDIR="$tmp/quote-stage" "$fixture/deploy.sh" install > "$tmp/output" 2>&1
    lacks SUDO; lacks 'MODE restore'
    [ -x "$tmp/quote-stage$rejected_prefix/bin/lerwatt" ] || die 'staged prefix was incorrectly rejected'
done

# Control characters cannot inject another unit/config line.
for bad_prefix in "$(printf '/opt/line\nbreak')" "$(printf '/opt/tab\tvalue')" "$(printf '/opt/cr\rvalue')"; do
    reset_log
    expect_status 1 env PREFIX="$bad_prefix" "$fixture/deploy.sh" install
    lacks BUILD; lacks SUDO
done
printf 'test_deploy: all checks passed (fixtures only; no host service or CPU changes)\n'
