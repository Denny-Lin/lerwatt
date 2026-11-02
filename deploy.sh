#!/bin/sh
# SPDX-License-Identifier: MIT
# Native tools, supervised trials and a persistent systemd service.
set -eu

fail() { printf 'lerwatt: %s\n' "$*" >&2; exit 1; }
usage()
{
    cat <<'EOF'
Usage: ./deploy.sh COMMAND [OPTIONS]
       lerwatt COMMAND [OPTIONS]          (after installation)

  install       Install, enable and start the persistent systemd service.
  install-tools Install commands only; no service or boot changes.
  uninstall     Stop/disable the owned service, restore, then remove tools/unit.
  status        Show persistent service state.
  start         Start the installed service.
  stop          Stop the service and restore its journaled settings.
  restart       Restart the service, restoring between worker processes.
  logs          Show the most recent service journal messages.
  check         Check real hardware without changing CPU settings.
  preview       Learn/log without changing CPU control settings.
  run           Foreground trial; MPC for 300 seconds by default.
  restore       Restore a stopped trial's journaled settings.
  help          Show this help.

check, preview and run accept:
  --policy rule|mpc          --prediction nominal|cooperative          --duration SECONDS
  --period-ms MILLISECONDS   --soft-limit CELSIUS
  --temperature-sensor PATH  --telemetry FILE
install also accepts --policy, --prediction, --period-ms, --soft-limit and
--temperature-sensor to configure the persistent service.
  --lertick                 Enable optional Lertick coordination (BPF build).
  --no-lertick              Disable coordination (default for a new install).
An upgrade preserves its existing coordination setting unless overridden.
Explicit --lertick defaults to cooperative prediction; --no-lertick defaults
to nominal. An explicit --prediction overrides either choice, in any order.
Without these flags, upgrades preserve the saved prediction mode.
Coordination requires a control period of 10..100 ms (default 100 ms).

The service runs continuously, starts at boot and restarts after failures.
An independent systemd watchdog stops an unresponsive worker; ExecStopPost
attempts restoration after it has stopped. Kernel thermal protection stays on.
Service logs use journald. Models are stored privately under /var/lib/lerwatt.
Configuration: /etc/lerwatt/lerwatt.conf (strict KEY=VALUE, never shell code).
Known competing policy services are reported, never automatically stopped.

PREFIX defaults to /usr/local (or the installed helper's prefix).
DESTDIR stages install/install-tools/uninstall without running sudo, a daemon
or systemctl. Without systemd use install-tools and your own service manager.
EOF
}
case $0 in */*) script=$0 ;; *) script=$(command -v "$0") || fail 'cannot locate helper' ;; esac
tool_dir=$(CDPATH= cd -P "$(dirname "$script")" && pwd)
script=$tool_dir/$(basename "$script")
source_tree=no
if [ -f "$tool_dir/Makefile" ] && [ -f "$tool_dir/src/lerwattd.c" ]; then
    source_tree=yes
    daemon=$tool_dir/build/lerwattd-standalone
    prefix=${PREFIX:-/usr/local}
else
    installed_prefix=$(CDPATH= cd -P "$tool_dir/.." && pwd)
    daemon=$installed_prefix/sbin/lerwattd
    prefix=${PREFIX:-$installed_prefix}
fi
destdir=${DESTDIR:-}
unit=/etc/systemd/system/lerwatt.service
config_dir=/etc/lerwatt
config=$config_dir/lerwatt.conf
state_dir=/var/lib/lerwatt
service_marker="# Managed by Lerwatt deployment: $prefix"
command=${1:-help}
install_contract=
if [ "$#" -gt 0 ]; then shift; fi
case $command in
    help|--help|-h) usage; exit 0 ;;
    check|preview|run|restore|install|install-tools|uninstall|status|start|stop|restart|logs|install-locked|install-tools-locked|uninstall-locked|service-run|service-restore|service-check|config-contract) ;;
    *) usage >&2; fail "unknown command: $command" ;;
esac
validate_path()
{
    case $2 in /*) ;; *) fail "$1 must be absolute" ;; esac
    case $2 in *[!/]*) ;; *) fail "$1 must not resolve to /" ;; esac
    case "$2/" in */../*|*/./*) fail "$1 must not contain . or .. components" ;; esac
    case $2 in *'
'*) fail "$1 must not contain control characters" ;; esac
    if printf '%s' "$2" | LC_ALL=C grep '[[:cntrl:]]' >/dev/null; then
        fail "$1 must not contain control characters"
    fi
}
validate_path PREFIX "$prefix"
if [ -n "$destdir" ]; then
    validate_path DESTDIR "$destdir"
    case $command in install|install-tools|uninstall) ;; *) fail 'DESTDIR only supports file staging' ;; esac
fi
# systemd rejects quote/backslash characters in the executable pathname even
# after unit-file unquoting. Tools-only and staged paths do not execute a unit.
case $command in
    install|install-locked)
        if [ -z "$destdir" ]; then
            case $prefix in
                *"'"*|*'"'*|*'\'*) fail 'persistent-service PREFIX must not contain quotes or backslashes; use a protected simple path such as /usr/local or /opt/lerwatt' ;;
            esac
        fi ;;
esac

validate_value()
{
    [ -n "$2" ] || fail "$1 needs a value"
    case $2 in *'
'*) fail "$1 must not contain control characters" ;; esac
    if printf '%s' "$2" | LC_ALL=C grep '[[:cntrl:]]' >/dev/null; then
        fail "$1 must not contain control characters"
    fi
    case $1 in
        --policy) case $2 in rule|mpc) ;; *) fail '--policy must be rule or mpc' ;; esac ;;
        --prediction) case $2 in nominal|cooperative) ;; *) fail '--prediction must be nominal or cooperative' ;; esac ;;
        --duration|--period-ms)
            case $2 in *[!0-9]*) fail "$1 must be a positive integer" ;; esac
            [ "$2" -gt 0 ] 2>/dev/null || fail "$1 must be a positive integer"
            if [ "$1" = --period-ms ]; then
                [ "$2" -ge 10 ] && [ "$2" -le 10000 ] || fail '--period-ms must be 10..10000'
            fi ;;
        --soft-limit)
            case $2 in *[!0-9.]*|.*|*.|*.*.*) fail '--soft-limit must be a positive decimal' ;; esac
            case $2 in *[1-9]*) ;; *) fail '--soft-limit must be positive' ;; esac ;;
        --temperature-sensor) case $2 in /sys/*) ;; *) fail '--temperature-sensor must be an absolute /sys/... input' ;; esac ;;
        --telemetry) ;;
    esac
}
validate_options()
{
    while [ "$#" -gt 0 ]; do
        case $1 in
            --lertick|--no-lertick)
                case $command in install|install-locked) ;; *) fail "$1 is for persistent installation only" ;; esac
                case $1 in --lertick) value=auto ;; *) value=off ;; esac
                [ -z "$install_contract" ] || [ "$install_contract" = "$value" ] || fail 'choose only one of --lertick and --no-lertick'
                install_contract=$value; shift; continue ;;
            --policy|--prediction|--duration|--period-ms|--soft-limit|--temperature-sensor|--telemetry)
                [ "$#" -ge 2 ] || fail "$1 needs a value"
                option=$1; value=$2; shift 2 ;;
            --policy=*|--prediction=*|--duration=*|--period-ms=*|--soft-limit=*|--temperature-sensor=*|--telemetry=*)
                option=${1%%=*}; value=${1#*=}; shift ;;
            *) fail "unsupported option: $1 (hardware root is fixed)" ;;
        esac
        if [ "$command" = install ] || [ "$command" = install-locked ]; then
            case $option in --duration|--telemetry) fail "$option is for foreground trials only" ;; esac
        fi
        validate_value "$option" "$value"
    done
}
case $command in
    check|preview|run|install|install-locked) validate_options "$@" ;;
    *) [ "$#" -eq 0 ] || fail "$command does not accept options" ;;
esac
[ "$(uname -s)" = Linux ] || fail 'hardware deployment requires Linux'
trusted_root_path()
{
    trust_remaining=${1#/}; trust_path=/
    while :; do
        [ ! -L "$trust_path" ] || fail "symlink in privileged path: $trust_path"
        if [ -e "$trust_path" ]; then
            trust_stat=$(stat -c '%u %a' "$trust_path") || fail "cannot inspect $trust_path"
            set -- $trust_stat
            [ "$#" -eq 2 ] && [ "$1" = 0 ] || fail "privileged path must be root-owned: $trust_path"
            case $2 in ''|*[!0-7]*) fail "invalid permissions: $trust_path" ;; esac
            [ "$((0$2 & 022))" -eq 0 ] || fail "privileged path must not be group/other writable: $trust_path"
            if [ -n "$trust_remaining" ]; then [ -d "$trust_path" ] || fail "ancestor is not a directory: $trust_path"; fi
        fi
        [ -n "$trust_remaining" ] || break
        trust_component=${trust_remaining%%/*}
        case $trust_remaining in */*) trust_remaining=${trust_remaining#*/} ;; *) trust_remaining= ;; esac
        [ -n "$trust_component" ] || continue
        trust_path=${trust_path%/}/$trust_component
    done
}
trusted_installation() { trusted_root_path "$1/bin"; trusted_root_path "$1/sbin"; }
ensure_daemon()
{
    if [ "$source_tree" = yes ]; then
        build_target=build/lerwattd-standalone
        case $command in install|install-locked)
            if [ "$install_contract" = auto ]; then build_target=build/lerwattd; fi ;;
        esac
        daemon=$tool_dir/$build_target
        if [ "$(id -u)" -ne 0 ]; then
            command -v make >/dev/null 2>&1 || fail 'install Make and a C11 compiler first'
            if ! make -C "$tool_dir" "$build_target"; then
                if [ "$install_contract" = auto ]; then
                    fail 'BPF-enabled build failed; usable Linux userspace BPF headers are required (Ubuntu: linux-libc-dev); installation was kept'
                fi
                fail 'local build failed; installation was kept'
            fi
        elif [ ! -x "$daemon" ]; then
            fail "build as your normal user first: make $build_target"
        fi
    fi
    [ -f "$daemon" ] && [ -x "$daemon" ] && [ ! -L "$daemon" ] || fail "daemon not found: $daemon"
    if [ -z "$destdir" ]; then
        if [ "$source_tree" = no ] && [ "$(id -u)" -eq 0 ]; then trusted_root_path "$daemon"; fi
        "$daemon" --help >/dev/null || fail 'daemon cannot run; rebuild locally'
    fi
}
become_root()
{
    if [ "$(id -u)" -ne 0 ]; then
        command -v sudo >/dev/null 2>&1 || fail 'sudo is unavailable; build first then invoke as root'
        # Preserve the actual CLI: synthesizing --lertick/--no-lertick from
        # saved configuration would incorrectly apply a new prediction default
        # on plain upgrade. The privileged reentry validates the config again
        # and checks the selected binary's capabilities before stopping service.
        exec sudo env PREFIX="$prefix" DESTDIR="$destdir" "$script" "$command" "$@"
    fi
}
require_systemd()
{
    command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ] ||
        fail 'persistent installation requires running systemd; use install-tools for other init systems'
    systemctl list-units --no-pager --no-legend >/dev/null 2>&1 || fail 'cannot contact the systemd manager'
}
owned_unit()
{
    [ -e "$unit" ] || [ -L "$unit" ] || return 1
    trusted_root_path "$unit"
    [ -f "$unit" ] && [ ! -L "$unit" ] || fail 'refusing unexpected service unit path'
    IFS= read -r unit_header < "$unit" || fail 'cannot read service ownership marker'
    [ "$unit_header" = "$service_marker" ] || fail 'lerwatt.service belongs to another installation; uninstall that installation first'
    return 0
}
check_service_slot()
{
    trusted_root_path "$unit"
    if owned_unit; then :; fi
    fragment=$(systemctl show lerwatt.service --property FragmentPath --value 2>/dev/null || :)
    [ -z "$fragment" ] || [ "$fragment" = "$unit" ] || fail "refusing foreign lerwatt.service: $fragment"
    dropins=$(systemctl show lerwatt.service --property DropInPaths --value 2>/dev/null || :)
    [ -z "$dropins" ] || fail 'remove or review custom lerwatt.service drop-ins before replacing the managed service'
}
check_competing_services()
{
    for manager in tlp.service auto-cpufreq.service tuned.service power-profiles-daemon.service; do
        if systemctl is-active --quiet "$manager"; then
            fail "control conflict: $manager is active; choose one CPU power-policy owner before starting Lerwatt (no service was stopped automatically)"
        fi
    done
}
load_service_config()
{
    service_policy=mpc; service_prediction=nominal; service_period=100; service_soft=auto; service_sensor=auto; service_contract=off
    if [ -e "$config" ] || [ -L "$config" ]; then
        trusted_root_path "$config"
        [ -f "$config" ] && [ ! -L "$config" ] || fail 'service config must be a regular file'
        [ "$(stat -c '%h' "$config")" = 1 ] || fail 'service config must not be hardlinked'
        [ "$(wc -c < "$config")" -le 16384 ] || fail 'service config is too large'
        while IFS= read -r config_line || [ -n "$config_line" ]; do
            case $config_line in ''|'#'*) continue ;; esac
            case $config_line in *=*) key=${config_line%%=*}; value=${config_line#*=} ;; *) fail 'service config requires KEY=VALUE lines' ;; esac
            case $key in
                policy) validate_value --policy "$value"; service_policy=$value ;;
                prediction) validate_value --prediction "$value"; service_prediction=$value ;;
                period_ms) validate_value --period-ms "$value"; service_period=$value ;;
                soft_limit_c) if [ "$value" != auto ]; then validate_value --soft-limit "$value"; fi; service_soft=$value ;;
                temperature_sensor) if [ "$value" != auto ]; then validate_value --temperature-sensor "$value"; fi; service_sensor=$value ;;
                contract) case $value in off|auto) service_contract=$value ;; *) fail 'contract must be off or auto' ;; esac ;;
                *) fail "unknown service config key: $key" ;;
            esac
        done < "$config"
    fi
    validate_service_mode
}
validate_service_mode()
{
    if [ "$service_prediction" = cooperative ]; then
        [ "$service_policy" = mpc ] || fail 'cooperative prediction requires policy=mpc'
        [ "$service_period" -le 100 ] || fail 'cooperative prediction requires period_ms=10..100'
    fi
    if [ "$service_contract" = auto ]; then
        [ "$service_period" -le 100 ] || fail 'Lertick coordination requires period_ms=10..100; choose --period-ms 100 or disable coordination'
    fi
}
service_overrides()
{
    prediction_override=no; prediction_default=
    while [ "$#" -gt 0 ]; do
        case $1 in
            --lertick) service_contract=auto; prediction_default=cooperative; shift; continue ;;
            --no-lertick) service_contract=off; prediction_default=nominal; shift; continue ;;
        esac
        case $1 in *=*) option=${1%%=*}; value=${1#*=}; shift ;; *) option=$1; value=$2; shift 2 ;; esac
        case $option in
            --policy) service_policy=$value ;;
            --prediction) service_prediction=$value; prediction_override=yes ;;
            --period-ms) service_period=$value ;;
            --soft-limit) service_soft=$value ;;
            --temperature-sensor) service_sensor=$value ;;
        esac
    done
    # Only an explicit installation-mode selection changes the default.
    # A detected/running peer never selects a model, and plain upgrades keep
    # saved choices (including explicit opt-outs and legacy nominal configs).
    if [ "$prediction_override" = no ] && [ -n "$prediction_default" ]; then
        service_prediction=$prediction_default
    fi
    validate_service_mode
}
select_install_contract()
{
    # Configuration is root-private. Read only its strictly validated mode,
    # before the normal user's build. The privileged reentry reads it again;
    # a stored choice must not become an explicit installation-mode override.
    if [ -z "$install_contract" ]; then
        if [ "$(id -u)" -eq 0 ]; then
            load_service_config
            install_contract=$service_contract
        else
            command -v sudo >/dev/null 2>&1 || fail 'sudo is unavailable; cannot read the existing service configuration'
            install_contract=$(sudo env PREFIX="$prefix" DESTDIR= "$script" config-contract) || fail 'cannot read the existing service configuration; installation was kept'
        fi
    fi
    case $install_contract in off|auto) ;; *) fail 'invalid coordination mode from service configuration' ;; esac
}
require_contract_capability()
{
    if [ "$service_contract" = auto ]; then
        build_features=$("$daemon" --build-features) || fail 'cannot inspect daemon build capabilities; rebuild from the trusted checkout'
        [ "$build_features" = contract=bpf ] || fail 'Lertick coordination requires the BPF-enabled daemon; run ./deploy.sh install --lertick from the trusted checkout'
    fi
}
write_service_files()
{
    trusted_root_path "$config_dir"; trusted_root_path "$state_dir"
    umask 077
    mkdir -p "$config_dir" "$state_dir"
    chmod 700 "$config_dir" "$state_dir"
    trusted_root_path "$config_dir"; trusted_root_path "$state_dir"
    config_tmp=$(mktemp "$config_dir/.config.XXXXXX")
    printf '# Lerwatt persistent service configuration. Strict KEY=VALUE; no shell syntax.\npolicy=%s\nprediction=%s\nperiod_ms=%s\nsoft_limit_c=%s\ntemperature_sensor=%s\ncontract=%s\n' \
        "$service_policy" "$service_prediction" "$service_period" "$service_soft" "$service_sensor" "$service_contract" > "$config_tmp"
    chmod 600 "$config_tmp"
    mv -f "$config_tmp" "$config"
    # Escape unit quoting and percent specifiers. The executable token is
    # resolved before environment expansion; a literal dollar must stay single.
    unit_helper=$(printf '%s' "$prefix/bin/lerwatt" | sed 's/\\/\\\\/g; s/"/\\"/g; s/%/%%/g')
    unit_tmp=$(mktemp "${unit%/*}/.lerwatt.service.XXXXXX")
    cat > "$unit_tmp" <<EOF
$service_marker
[Unit]
Description=Lerwatt persistent AI CPU power manager
Documentation=https://github.com/Denny-Lin/lerwatt
After=local-fs.target power-profiles-daemon.service tlp.service auto-cpufreq.service tuned.service thermald.service
StartLimitIntervalSec=0

[Service]
Type=notify
NotifyAccess=main
ExecStartPre="$unit_helper" service-check
ExecStart="$unit_helper" service-run
ExecStopPost="$unit_helper" service-restore
WatchdogSec=30s
WatchdogSignal=SIGTERM
Restart=always
RestartSec=10s
TimeoutStartSec=60s
TimeoutStopSec=15s
TimeoutAbortSec=15s
KillMode=control-group
UMask=0077
StateDirectory=lerwatt
StateDirectoryMode=0700
LogRateLimitIntervalSec=30s
LogRateLimitBurst=500
LimitCORE=0
NoNewPrivileges=yes
ProtectSystem=full
ProtectHome=read-only
PrivateTmp=yes
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
    chmod 644 "$unit_tmp"
    mv -f "$unit_tmp" "$unit"
}
maintenance()
{
    old_daemon=$prefix/sbin/lerwattd
    if [ -e "$old_daemon" ] || [ -L "$old_daemon" ]; then
        [ -f "$old_daemon" ] && [ -x "$old_daemon" ] && [ ! -L "$old_daemon" ] || fail 'unexpected installed daemon'
        trusted_root_path "$old_daemon"
    fi
    if ! "$daemon" --sysroot / --no-contract --restore -- "$script" "$command-locked" "$@"; then
        if [ "$command" = uninstall ]; then
            fail 'restoration failed; the service is stopped/disabled and tools, unit and recovery data were kept for repair'
        fi
        fail 'restoration or maintenance failed; installed files/recovery data were kept'
    fi
}
case $command in
    config-contract)
        [ "$(id -u)" -eq 0 ] && [ -z "$destdir" ] || fail 'configuration inspection requires root and no DESTDIR'
        load_service_config
        printf '%s\n' "$service_contract"
        exit 0 ;;
    status)
        require_systemd; become_root; load_service_config
        printf 'Lertick coordination configured: %s (connection state is reported in lerwatt logs).\n' "$service_contract"
        printf 'Prediction mode configured: %s (reliability checks gate individual corrections).\n' "$service_prediction"
        systemctl is-enabled lerwatt.service || :
        exec systemctl status lerwatt.service --no-pager ;;
    start|stop|restart|logs)
        require_systemd; become_root; check_service_slot
        owned_unit || fail 'the managed service is not installed'
        case $command in
            logs) exec journalctl -u lerwatt.service -n 100 --no-pager ;;
            start|restart) check_competing_services ;;
        esac
        exec systemctl "$command" lerwatt.service ;;
    service-run|service-restore|service-check)
        [ "$(id -u)" -eq 0 ] && [ -z "$destdir" ] || fail 'service operations require root'
        ensure_daemon
        if [ "$command" = service-restore ]; then exec "$daemon" --sysroot / --no-contract --restore; fi
        require_systemd; check_competing_services; load_service_config
        require_contract_capability
        set -- --sysroot / --policy "$service_policy" --prediction "$service_prediction" --period-ms "$service_period"
        if [ "$service_contract" = off ]; then set -- "$@" --no-contract; fi
        if [ "$service_soft" != auto ]; then set -- "$@" --soft-limit "$service_soft"; fi
        if [ "$service_sensor" != auto ]; then set -- "$@" --temperature-sensor "$service_sensor"; fi
        if [ "$command" = service-check ]; then exec "$daemon" "$@" --check; fi
        exec "$daemon" "$@" --duration 0 --watchdog --model-state "$state_dir/model.state" ;;
    install|install-tools|uninstall)
        if [ -z "$destdir" ]; then
            if [ "$command" = install ]; then select_install_contract; fi
            if [ "$command" = uninstall ] && [ -x "$prefix/sbin/lerwattd" ]; then
                daemon=$prefix/sbin/lerwattd
            else
                ensure_daemon
            fi
            become_root "$@"
            trusted_installation "$prefix"
            if [ "$command" = install ]; then
                require_systemd; check_service_slot; check_competing_services
                load_service_config; service_overrides "$@"
                service_contract=$install_contract
                require_contract_capability
                # Check requested inputs before stopping/replacing a running service.
                set -- --policy "$service_policy" --prediction "$service_prediction" --period-ms "$service_period"
                if [ "$service_soft" != auto ]; then set -- "$@" --soft-limit "$service_soft"; fi
                if [ "$service_sensor" != auto ]; then set -- "$@" --temperature-sensor "$service_sensor"; fi
                "$daemon" --sysroot / --no-contract "$@" --check || fail 'hardware check failed; service was not installed'
                # Re-use the validated settings as explicit maintenance overrides.
                case $service_contract in auto) set -- "$@" --lertick ;; off) set -- "$@" --no-lertick ;; esac
                if owned_unit; then systemctl stop lerwatt.service || fail 'service did not stop; installation was kept'; fi
                maintenance "$@"
                systemctl daemon-reload
                systemctl enable --now lerwatt.service || fail 'service activation failed; inspect lerwatt logs; files and recovery data were kept'
                systemctl is-active --quiet lerwatt.service || fail 'service did not stay active after startup; inspect lerwatt logs; installation was kept'
                printf 'Lerwatt is enabled at boot and running continuously until stopped or uninstalled.\n'
            elif [ "$command" = uninstall ]; then
                if owned_unit; then
                    require_systemd; check_service_slot
                    systemctl stop lerwatt.service || fail 'service did not stop; installed tools were kept'
                    systemctl disable lerwatt.service || fail 'service could not be disabled; installed tools were kept'
                    maintenance
                    systemctl daemon-reload
                else
                    maintenance
                fi
            else
                maintenance
            fi
            exit 0
        fi ;;
    install-locked|install-tools-locked|uninstall-locked)
        [ "$(id -u)" -eq 0 ] && [ -z "$destdir" ] || fail 'internal maintenance requires root and no DESTDIR' ;;
esac
case $command in
    install|install-tools|install-locked|install-tools-locked)
        ensure_daemon
        target=$destdir$prefix
        if [ "$(id -u)" -eq 0 ]; then trusted_installation "$target"; fi
        for path in "$target/bin/lerwatt" "$target/sbin/lerwattd"; do
            if [ -L "$path" ] || { [ -e "$path" ] && [ ! -f "$path" ]; }; then fail "refusing nonregular install target: $path"; fi
        done
        umask 022
        mkdir -p "$target/bin" "$target/sbin"
        if [ "$(id -u)" -eq 0 ]; then trusted_installation "$target"; fi
        daemon_tmp=$(mktemp "$target/sbin/.lerwattd.XXXXXX")
        helper_tmp=$(mktemp "$target/bin/.lerwatt.XXXXXX") || { rm -f "$daemon_tmp"; exit 1; }
        trap 'rm -f "$daemon_tmp" "$helper_tmp"' 0
        cp "$daemon" "$daemon_tmp"; cp "$script" "$helper_tmp"
        chmod 755 "$daemon_tmp" "$helper_tmp"
        mv -f "$daemon_tmp" "$target/sbin/lerwattd"
        mv -f "$helper_tmp" "$target/bin/lerwatt"
        trap - 0
        if [ "$command" = install-locked ]; then
            check_service_slot; load_service_config; service_overrides "$@"; require_contract_capability; write_service_files
            printf 'Installed commands and the persistent service unit.\n'
        else
            printf 'Installed %s and %s; no service was started or enabled.\n' "$target/bin/lerwatt" "$target/sbin/lerwattd"
        fi
        exit 0 ;;
    uninstall|uninstall-locked)
        target=$destdir$prefix
        if [ "$(id -u)" -eq 0 ]; then trusted_installation "$target"; fi
        if [ "$command" = uninstall-locked ] && owned_unit; then rm -f "$unit"; fi
        rm -f "$target/bin/lerwatt" "$target/sbin/lerwattd"
        printf 'Removed managed tools/unit. Configuration, model state and recovery data were kept.\n'
        exit 0 ;;
esac
ensure_daemon
become_root "$@"
case $command in
    check) exec "$daemon" --sysroot / --no-contract --policy mpc "$@" --check ;;
    preview) exec "$daemon" --sysroot / --no-contract --policy mpc --duration 300 --dry-run "$@" ;;
    restore) exec "$daemon" --sysroot / --no-contract --restore ;;
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
