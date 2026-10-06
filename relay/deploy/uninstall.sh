#!/usr/bin/env bash
# uninstall.sh - remove the HIDway relay from this Pi.
#
#   sudo relay/deploy/uninstall.sh            stop and remove services, udev rule, commands
#   sudo relay/deploy/uninstall.sh --purge    also remove /opt/hidway, /etc/hidway and the users
#
# The configuration is backed up to /var/backups/hidway/<timestamp>/ first;
# backups are never removed.
set -euo pipefail
umask 022

ROOT=${HIDWAY_ROOT:-} # prefix, for testing only
SYSTEMCTL=${HIDWAY_SYSTEMCTL:-systemctl}
UDEVADM=${HIDWAY_UDEVADM:-udevadm}
BACKUPS=$ROOT/var/backups/hidway
STAMP=$(date +%Y%m%d-%H%M%S)-uninstall

say() { printf '==> %s\n' "$*"; }
die() { printf 'uninstall.sh: error: %s\n' "$*" >&2; exit 1; }

main() {
    local purge=0 f
    case ${1:-} in
    '') ;;
    --purge) purge=1 ;;
    *) die "unknown option: $1" ;;
    esac
    [[ $(id -u) == 0 || -n $ROOT ]] || die "run as root: sudo $0 $*"

    say "stopping services"
    "$SYSTEMCTL" disable --now hidwayd.service hidway-update.timer 2>/dev/null || true

    if [[ -d $ROOT/etc/hidway ]]; then
        install -d -m 0700 "$BACKUPS"
        mkdir -p "$BACKUPS/$STAMP/etc"
        cp -a "$ROOT/etc/hidway" "$BACKUPS/$STAMP/etc/"
        say "configuration backed up to ${BACKUPS#"$ROOT"}/$STAMP/"
    fi

    say "removing service files"
    for f in etc/systemd/system/hidwayd.service etc/systemd/system/hidway-update.service \
        etc/systemd/system/hidway-update.timer etc/udev/rules.d/99-hidway.rules \
        etc/update-motd.d/60-hidway usr/local/bin/hidway-status usr/local/sbin/hidway-update; do
        rm -f "$ROOT/$f"
    done
    "$SYSTEMCTL" daemon-reload
    "$UDEVADM" control --reload-rules

    if [[ $purge == 1 ]]; then
        say "purging /opt/hidway, /etc/hidway and the hidway users"
        rm -rf "$ROOT/opt/hidway" "$ROOT/etc/hidway"
        if [[ -z $ROOT ]]; then
            for u in hidway-build hidway; do
                id -u "$u" >/dev/null 2>&1 && userdel "$u"
                getent group "$u" >/dev/null && groupdel "$u"
            done
        fi
    else
        say "kept /opt/hidway and /etc/hidway (use --purge to remove them)"
    fi
    say "done"
}

main "$@"
exit
