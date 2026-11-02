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
[ "$1" = -c ] && [ "$2" = '%u %a' ] || exit 1
if [ "$3" = "${UNTRUSTED_PATH:-}" ]; then
    printf '%s\n' "${UNTRUSTED_STAT:-1000 755}"
else
    printf '%s\n' '0 755'
fi
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
[ "$1" = -C ] && [ "$3" = build/lerwattd-standalone ] || exit 12
mkdir -p "$2/build"
cp "$STUB_TEMPLATE" "$2/build/lerwattd-standalone"
chmod 755 "$2/build/lerwattd-standalone"
EOF
cat > "$STUB_TEMPLATE" <<'EOF'
#!/bin/sh
set -eu
mode=run
for arg do
    case $arg in
        --check) mode=check ;;
        --restore) mode=restore ;;
        --dry-run) mode=preview ;;
        --help) mode=help ;;
    esac
done
printf 'MODE %s\n' "$mode" >> "$TEST_LOG"
for arg do printf 'ARG %s\n' "$arg" >> "$TEST_LOG"; done
case $mode in
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
chmod 755 "$tools/id" "$tools/uname" "$tools/stat" "$tools/sudo" "$tools/make" "$STUB_TEMPLATE"

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
PREFIX=/opt/lerwatt DESTDIR="$tmp/stage area" "$fixture/deploy.sh" install > "$tmp/output"
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
    expect_status 1 env PREFIX="$root_path" "$fixture/deploy.sh" install
    expect_status 1 env DESTDIR="$root_path" "$fixture/deploy.sh" install
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
PREFIX="$tmp/live prefix" "$fixture/deploy.sh" install > "$tmp/output" 2>&1
has SUDO
has 'MAINTENANCE BEGIN'
has 'MAINTENANCE END'
has 'ARG install-locked'
[ -x "$tmp/live prefix/bin/lerwatt" ] || die 'installed helper missing'
printf '\n# installed generation\n' >> "$tmp/live prefix/sbin/lerwattd"
cp "$tmp/live prefix/sbin/lerwattd" "$tmp/old-daemon"
reset_log
expect_status 1 env PREFIX="$tmp/live prefix" RESTORE_EXIT=23 "$fixture/deploy.sh" install
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
PREFIX="$tmp/legacy" "$fixture/deploy.sh" install > "$tmp/output" 2>&1
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
expect_status 1 env PREFIX="$tmp/incompatible" HELP_EXIT=126 "$fixture/deploy.sh" install
has 'MODE help'
lacks SUDO
lacks 'MODE restore'
grep -F -x 'keep this binary' "$tmp/incompatible/sbin/lerwattd" >/dev/null || die 'incompatible daemon replaced installed binary'

# A failed source build cannot replace an existing installation.
reset_log
mkdir -p "$tmp/keep/sbin"
printf 'keep this binary\n' > "$tmp/keep/sbin/lerwattd"
expect_status 1 env PREFIX="$tmp/keep" FAIL_BUILD=1 "$fixture/deploy.sh" install
grep -F -x 'keep this binary' "$tmp/keep/sbin/lerwattd" >/dev/null || die 'build failure replaced installed binary'
lacks SUDO

# Root installation must reject untrusted ancestors and recovery executables.
for ownership in '1000 755' '0 775' '0 757'; do
    reset_log
    mkdir -p "$tmp/untrusted/sbin"
    cp "$STUB_TEMPLATE" "$tmp/untrusted/sbin/lerwattd"
    chmod 755 "$tmp/untrusted/sbin/lerwattd"
    expect_status 1 env FAKE_UID=0 PREFIX="$tmp/untrusted" \
        UNTRUSTED_PATH="$tmp/untrusted" UNTRUSTED_STAT="$ownership" "$fixture/deploy.sh" install
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
expect_status 1 env FAKE_UID=0 PREFIX="$tmp/prefix-link" "$fixture/deploy.sh" install
lacks 'MODE restore'
reset_log
mkdir -p "$tmp/stage-link/opt"
ln -s "$tmp/untrusted" "$tmp/stage-link/opt/lerwatt"
expect_status 1 env FAKE_UID=0 PREFIX=/opt/lerwatt DESTDIR="$tmp/stage-link" "$fixture/deploy.sh" install
lacks 'MODE help'
lacks 'MODE restore'
reset_log
expect_status 1 env FAKE_UID=0 PREFIX=/opt/lerwatt DESTDIR="$tmp/root-stage" \
    UNTRUSTED_PATH="$tmp" UNTRUSTED_STAT='0 1777' "$fixture/deploy.sh" install
lacks 'MODE help'
lacks SUDO
[ ! -e "$tmp/root-stage" ] || die 'root staging wrote under untrusted ancestor'
reset_log
expect_status 1 "$fixture/deploy.sh" install-locked
expect_status 1 env DESTDIR="$tmp/stage" FAKE_UID=0 "$fixture/deploy.sh" uninstall-locked
lacks 'MODE restore'
lacks SUDO

printf 'test_deploy: all checks passed (stub daemon; no host deployment)\n'
