#!/bin/sh
# SPDX-License-Identifier: MIT
# A small, foreground-only helper for trying Lerwatt on real Linux hardware.
set -eu

fail()
{
    printf 'lerwatt: %s\n' "$*" >&2
    exit 1
}

usage()
{
    cat <<'EOF'
Usage: ./deploy.sh COMMAND [OPTIONS]
       lerwatt COMMAND [OPTIONS]          (after installation)

  check       Read-only hardware checks; report whether control can run.
  preview     Learn and log without changing CPU limits or the scheduler.
  run         Supervised foreground trial; defaults to MPC for 300 seconds.
  restore     Restore a previous hardware trial's journaled settings.
  install     Build locally, then install the daemon and this helper only.
  uninstall   Restore settings, then remove the installed daemon and helper.
  help        Show this help.

check, preview and run accept:
  --policy rule|mpc          --duration SECONDS
  --period-ms MILLISECONDS   --soft-limit CELSIUS
  --temperature-sensor PATH  --telemetry FILE

Hardware is always selected explicitly (--sysroot /), and Lertick contract
publication is disabled (--no-contract). Stop a foreground trial with Ctrl-C.
No dependencies are downloaded and no other power manager is stopped.

PREFIX defaults to /usr/local (or the installed helper's own prefix).
DESTDIR is supported only for install/uninstall staging; staging never runs
the daemon, invokes sudo or changes host services or kernel settings.
Installation does not start a service or enable anything at boot.
EOF
}

case $0 in
    */*) script=$0 ;;
    *) script=$(command -v "$0") || fail 'cannot locate this helper' ;;
esac
tool_dir=$(CDPATH= cd -P "$(dirname "$script")" && pwd) || exit 1
script=$tool_dir/$(basename "$script")
source_tree=no
if [ -f "$tool_dir/Makefile" ] && [ -f "$tool_dir/src/lerwattd.c" ]; then
    source_tree=yes
    daemon=$tool_dir/build/lerwattd-standalone
    prefix=${PREFIX:-/usr/local}
else
    installed_prefix=$(CDPATH= cd -P "$tool_dir/.." && pwd) || exit 1
    daemon=$installed_prefix/sbin/lerwattd
    prefix=${PREFIX:-$installed_prefix}
fi
destdir=${DESTDIR:-}
command=${1:-help}
if [ "$#" -gt 0 ]; then shift; fi

case $command in
    help|--help|-h) usage; exit 0 ;;
    check|preview|run|restore|install|uninstall|install-locked|uninstall-locked) ;;
    *) usage >&2; fail "unknown command: $command" ;;
esac

validate_path()
{
    case $2 in
        /*) ;;
        *) fail "$1 must be an absolute path" ;;
    esac
    case $2 in
        *[!/]*) ;;
        *) fail "$1 must not resolve to /" ;;
    esac
    case "$2/" in
        */../*|*/./*) fail "$1 must not contain . or .. components" ;;
    esac
}
validate_path PREFIX "$prefix"
if [ -n "$destdir" ]; then
    validate_path DESTDIR "$destdir"
    case $command in
        install|uninstall) ;;
        *) fail 'DESTDIR is only for install/uninstall staging; unset it for hardware commands' ;;
    esac
fi

validate_value()
{
    [ -n "$2" ] || fail "$1 needs a value"
    case $1 in
        --policy)
            case $2 in rule|mpc) ;; *) fail '--policy must be rule or mpc' ;; esac ;;
        --duration|--period-ms)
            case $2 in *[!0-9]*) fail "$1 must be a positive integer" ;; esac
            [ "$2" -gt 0 ] 2>/dev/null || fail "$1 must be a positive integer"
            if [ "$1" = --period-ms ]; then
                [ "$2" -ge 10 ] && [ "$2" -le 10000 ] || fail '--period-ms must be 10..10000'
            fi ;;
        --soft-limit)
            case $2 in *[!0-9.]*|.*|*.|*.*.*) fail '--soft-limit must be a positive decimal temperature' ;; esac
            case $2 in *[1-9]*) ;; *) fail '--soft-limit must be positive' ;; esac ;;
        --temperature-sensor)
            case $2 in /*) ;; *) fail '--temperature-sensor must be an absolute path' ;; esac ;;
        --telemetry) ;;
    esac
}

validate_options()
{
    while [ "$#" -gt 0 ]; do
        case $1 in
            --policy|--duration|--period-ms|--soft-limit|--temperature-sensor|--telemetry)
                [ "$#" -ge 2 ] || fail "$1 needs a value"
                validate_value "$1" "$2"
                shift 2 ;;
            --policy=*|--duration=*|--period-ms=*|--soft-limit=*|--temperature-sensor=*|--telemetry=*)
                validate_value "${1%%=*}" "${1#*=}"
                shift ;;
            *) fail "unsupported option: $1 (hardware root and standalone mode are fixed)" ;;
        esac
    done
}

case $command in
    check|preview|run) validate_options "$@" ;;
    *) [ "$#" -eq 0 ] || fail "$command does not accept options" ;;
esac

[ "$(uname -s)" = Linux ] || fail 'hardware deployment requires Linux'

# Installed programs and their directories must not be replaceable by another
# local user. Linux coreutils and BusyBox both provide this stat format.
trusted_root_path()
{
    trust_remaining=${1#/}
    trust_path=/
    while :; do
        [ ! -L "$trust_path" ] || fail "refusing a symlink in privileged installation path: $trust_path"
        if [ -e "$trust_path" ]; then
            trust_stat=$(stat -c '%u %a' "$trust_path") || fail "cannot inspect installation path: $trust_path"
            set -- $trust_stat
            [ "$#" -eq 2 ] || fail "unexpected stat output for: $trust_path"
            [ "$1" = 0 ] || fail "privileged installation path must be root-owned: $trust_path"
            case $2 in ''|*[!0-7]*) fail "invalid permissions for: $trust_path" ;; esac
            [ "$((0$2 & 022))" -eq 0 ] || fail "privileged installation path must not be group/other writable: $trust_path"
            if [ -n "$trust_remaining" ]; then
                [ -d "$trust_path" ] || fail "installation ancestor is not a directory: $trust_path"
            fi
        fi
        [ -n "$trust_remaining" ] || break
        trust_component=${trust_remaining%%/*}
        case $trust_remaining in
            */*) trust_remaining=${trust_remaining#*/} ;;
            *) trust_remaining= ;;
        esac
        [ -n "$trust_component" ] || continue
        trust_path=${trust_path%/}/$trust_component
    done
}

trusted_installation()
{
    trusted_root_path "$1/bin"
    trusted_root_path "$1/sbin"
}

ensure_daemon()
{
    if [ "$source_tree" = yes ]; then
        if [ "$(id -u)" -ne 0 ]; then
            command -v make >/dev/null 2>&1 || fail 'install Make and a C11 compiler using your distribution, then retry'
            make -C "$tool_dir" build/lerwattd-standalone || fail 'local build failed; no installation was changed'
        elif [ ! -x "$daemon" ]; then
            fail 'build as your normal user first: make build/lerwattd-standalone; then retry'
        fi
    fi
    [ -f "$daemon" ] && [ -x "$daemon" ] || fail "daemon not found: $daemon"
    if [ -z "$destdir" ]; then
        if [ "$source_tree" = no ] && [ "$(id -u)" -eq 0 ]; then
            trusted_root_path "$daemon"
        fi
        "$daemon" --help >/dev/null ||
            fail 'daemon cannot run on this Linux system; rebuild locally before retrying'
    fi
}

become_root()
{
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null 2>&1 || fail 'sudo is unavailable; run this command as root after building as your normal user'
        exec sudo env PREFIX="$prefix" DESTDIR="$destdir" "$script" "$command" "$@"
    fi
}

maintenance()
{
    old_daemon=$prefix/sbin/lerwattd
    if [ -e "$old_daemon" ] || [ -L "$old_daemon" ]; then
        [ -f "$old_daemon" ] && [ -x "$old_daemon" ] && [ ! -L "$old_daemon" ] ||
            fail "refusing to execute an unexpected installed daemon: $old_daemon"
        trusted_root_path "$old_daemon"
    fi
    # The locally built/current daemon implements lock-held maintenance. Older
    # installed versions may ignore extra restore arguments, so do not use them.
    "$daemon" --sysroot / --no-contract --restore -- "$script" "$command-locked" ||
        fail 'restoration or maintenance failed, or a controller is active; inspect the error above before retrying'
    exit 0
}

case $command in
    install|uninstall)
        if [ -z "$destdir" ]; then
            ensure_daemon
            become_root
            trusted_installation "$prefix"
            maintenance
        fi ;;
    install-locked|uninstall-locked)
        [ "$(id -u)" -eq 0 ] && [ -z "$destdir" ] ||
            fail 'internal maintenance commands require root and no DESTDIR' ;;
esac

case $command in
    install|install-locked)
        ensure_daemon
        target=$destdir$prefix
        if [ "$(id -u)" -eq 0 ]; then trusted_installation "$target"; fi
        for path in "$target/bin/lerwatt" "$target/sbin/lerwattd"; do
            if [ -L "$path" ] || { [ -e "$path" ] && [ ! -f "$path" ]; }; then
                fail "refusing to replace a non-regular installation path: $path"
            fi
        done
        # Do not create writable installation ancestors under a caller's 000 umask.
        umask 022
        mkdir -p "$target/bin" "$target/sbin"
        if [ "$(id -u)" -eq 0 ]; then trusted_installation "$target"; fi
        daemon_tmp=$(mktemp "$target/sbin/.lerwattd.XXXXXX") || exit 1
        helper_tmp=$(mktemp "$target/bin/.lerwatt.XXXXXX") || { rm -f "$daemon_tmp"; exit 1; }
        trap 'rm -f "$daemon_tmp" "$helper_tmp"' 0
        cp "$daemon" "$daemon_tmp"
        cp "$script" "$helper_tmp"
        chmod 755 "$daemon_tmp" "$helper_tmp"
        mv -f "$daemon_tmp" "$target/sbin/lerwattd"
        mv -f "$helper_tmp" "$target/bin/lerwatt"
        trap - 0
        printf 'Installed %s and %s\nNo service was started or enabled.\n' "$target/sbin/lerwattd" "$target/bin/lerwatt"
        exit 0 ;;
    uninstall|uninstall-locked)
        target=$destdir$prefix
        if [ "$(id -u)" -eq 0 ]; then trusted_installation "$target"; fi
        rm -f "$target/bin/lerwatt" "$target/sbin/lerwattd"
        printf 'Removed the helper and daemon from %s. Recovery data and telemetry were kept.\n' "$target"
        exit 0 ;;
esac

ensure_daemon
become_root "$@"

case $command in
    check)
        exec "$daemon" --sysroot / --no-contract --policy mpc "$@" --check ;;
    preview)
        # No restore here: an old journal must not make a preview actuate.
        exec "$daemon" --sysroot / --no-contract --policy mpc --duration 300 --dry-run "$@" ;;
    restore)
        exec "$daemon" --sysroot / --no-contract --restore ;;
    run) ;;
esac

"$daemon" --sysroot / --no-contract --policy mpc "$@" --check ||
    fail 'hardware check failed; no trial was started'

child=
pending_signal=
pending_status=0
finish_trial()
{
    trial_status=$1
    # Once the child has stopped, finish recovery even after repeated Ctrl-C.
    trap '' INT TERM HUP
    if ! "$daemon" --sysroot / --no-contract --restore; then
        printf 'lerwatt: restoration failed; keep this tool and the recovery journal, then retry restore\n' >&2
        trial_status=2
    fi
    exit "$trial_status"
}

stop_trial()
{
    signal=$1
    signal_status=$2
    if [ -z "$child" ]; then
        # A signal can land between launching the child and assigning $!.
        pending_signal=$signal
        pending_status=$signal_status
        return
    fi
    trap '' INT TERM HUP
    # POSIX asynchronous children may inherit SIGINT ignored before the daemon
    # installs its handlers. TERM reliably stops that early-startup child too.
    kill -TERM "$child" 2>/dev/null || :
    wait "$child" 2>/dev/null || :
    child=
    finish_trial "$signal_status"
}

trap 'stop_trial INT 130' INT
trap 'stop_trial TERM 143' TERM
trap 'stop_trial HUP 129' HUP
printf 'lerwatt: starting a foreground hardware trial; Ctrl-C stops it and restores settings\n' >&2
[ -z "$pending_signal" ] || finish_trial "$pending_status"
"$daemon" --sysroot / --no-contract --policy mpc --duration 300 "$@" &
child=$!
[ -z "$pending_signal" ] || stop_trial "$pending_signal" "$pending_status"
if wait "$child"; then trial_status=0; else trial_status=$?; fi
child=
finish_trial "$trial_status"
