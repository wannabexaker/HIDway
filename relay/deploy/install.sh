#!/usr/bin/env bash
# install.sh - set up the HIDway relay on a Raspberry Pi (Raspberry Pi OS / Debian).
#
#   sudo relay/deploy/install.sh [--ref BRANCH|latest-tag] [--repo URL]
#
# Safe to run again at any time. It never overwrites /etc/hidway/*.conf, and
# backs up every file it replaces (and the current config) under
# /var/backups/hidway/<timestamp>/ first.
#
# What it sets up:
#   users     hidway (runs the relay), hidway-build (fetches and builds)
#   /opt/hidway/src        checkout the Pi follows
#   /opt/hidway/releases   built releases; current/previous are symlinks
#   /etc/hidway/           hidwayd.conf, update.conf
#   systemd   hidwayd.service, hidway-update.timer (every 5 minutes)
#   udev      /dev/hidway-serial for the Raspberry Pi Debug Probe
#   command   /usr/local/sbin/hidway-update
#
# Internal: install.sh --refresh <dir> installs the service files found in
# <dir>; hidway-update --apply-system uses it.
set -euo pipefail
umask 022

ROOT=${HIDWAY_ROOT:-} # prefix, for testing only
SYSTEMCTL=${HIDWAY_SYSTEMCTL:-systemctl}
UDEVADM=${HIDWAY_UDEVADM:-udevadm}
HOME_DIR=$ROOT/opt/hidway
SRC=$HOME_DIR/src
ETC=$ROOT/etc/hidway
BACKUPS=$ROOT/var/backups/hidway
SERVICE_USER=hidway
BUILD_USER=${HIDWAY_BUILD_USER-hidway-build}
BACKUP_KEEP=10
STAMP=$(date +%Y%m%d-%H%M%S)

REPO=
REF=main
REPO_SET=0
REF_SET=0
CHANGED=0

say() { printf '==> %s\n' "$*"; }
die() { printf 'install.sh: error: %s\n' "$*" >&2; exit 1; }

as_build() {
    if [[ -n $BUILD_USER && $(id -u) == 0 ]]; then
        runuser -u "$BUILD_USER" -- env HOME=/nonexistent "$@"
    else
        "$@"
    fi
}

# Copy a file to <dest> atomically. An existing, different <dest> is backed up.
put() { # put <src> <dest> <mode>
    local src=$1 dest=$2 mode=$3
    if [[ -e $dest ]] && cmp -s "$src" "$dest"; then
        return 0
    fi
    [[ -e $dest ]] && backup "$dest"
    mkdir -p "$(dirname "$dest")"
    install -m "$mode" "$src" "$dest.new.$$"
    mv -f "$dest.new.$$" "$dest"
    say "installed ${dest#"$ROOT"}"
    CHANGED=1
}

backup() { # backup <path> -> /var/backups/hidway/<stamp>/<path>
    local rel=${1#"$ROOT"}
    mkdir -p "$BACKUPS/$STAMP$(dirname "$rel")"
    chmod 0700 "$BACKUPS"
    cp -a "$1" "$BACKUPS/$STAMP$rel"
}

prune_backups() {
    find "$BACKUPS" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' 2>/dev/null | sort -r |
        tail -n +$((BACKUP_KEEP + 1)) | while read -r d; do rm -rf -- "${BACKUPS:?}/$d"; done
}

ensure_user() { # ensure_user <name>
    getent group "$1" >/dev/null || groupadd --system "$1"
    id -u "$1" >/dev/null 2>&1 ||
        useradd --system --gid "$1" --home-dir /nonexistent --no-create-home \
            --shell /usr/sbin/nologin "$1"
}

# Install <example> as <dest> if missing; then apply KEY=value overrides.
put_conf() { # put_conf <example> <dest> [KEY=value...]
    local example=$1 dest=$2 tmp kv
    shift 2
    tmp=$(mktemp)
    if [[ -e $dest ]]; then cat "$dest" >"$tmp"; else cat "$example" >"$tmp"; fi
    for kv in "$@"; do
        sed -i "s|^${kv%%=*}=.*|$kv|" "$tmp"
    done
    put "$tmp" "$dest" 0644
    rm -f "$tmp"
}

# Service files: units, udev rule and the updater itself.
install_system_files() { # install_system_files <dir>
    local d=$1
    put "$d/hidwayd.service" "$ROOT/etc/systemd/system/hidwayd.service" 0644
    put "$d/hidway-update.service" "$ROOT/etc/systemd/system/hidway-update.service" 0644
    put "$d/hidway-update.timer" "$ROOT/etc/systemd/system/hidway-update.timer" 0644
    put "$d/99-hidway.rules" "$ROOT/etc/udev/rules.d/99-hidway.rules" 0644
    put "$d/hidway-update" "$ROOT/usr/local/sbin/hidway-update" 0755
    if [[ $CHANGED == 1 ]]; then
        "$SYSTEMCTL" daemon-reload
        "$UDEVADM" control --reload-rules
        "$UDEVADM" trigger --subsystem-match=tty
    fi
}

# All <placeholders> in hidwayd.conf replaced (comments aside).
# HTTPS URL of the checkout this script runs from (the build user has no SSH keys).
origin_url() {
    local url
    url=$(git -c safe.directory='*' -C "$1" remote get-url origin 2>/dev/null) || return 1
    case $url in
    git@*:*) url=https://${url#git@} && url=${url/://} ;;
    ssh://git@*) url=https://${url#ssh://git@} ;;
    esac
    printf '%s\n' "$url"
}

configured() { ! grep -v '^[[:space:]]*#' "$ETC/hidwayd.conf" | grep -q '<'; }

full_install() {
    local here
    here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

    local missing=() c
    for c in git make cc; do
        command -v "$c" >/dev/null || missing+=("$c")
    done
    ((${#missing[@]} == 0)) ||
        die "missing: ${missing[*]} (install with: sudo apt install git build-essential)"
    for c in runuser flock cmp useradd "$SYSTEMCTL" "$UDEVADM"; do
        command -v "$c" >/dev/null || missing+=("$c")
    done
    ((${#missing[@]} == 0)) || die "missing: ${missing[*]} (a systemd-based Linux is required)"

    say "users"
    ensure_user "$SERVICE_USER"
    [[ -n $BUILD_USER ]] && ensure_user "$BUILD_USER"

    say "directories"
    install -d -m 0755 "$HOME_DIR" "$HOME_DIR/releases" "$ETC"
    install -d -m 0700 "$BACKUPS"
    install -d -m 0755 ${BUILD_USER:+-o "$BUILD_USER" -g "$BUILD_USER"} "$SRC"

    # Repository: --repo, else the one already configured, else this checkout's origin.
    if [[ $REPO_SET == 0 ]]; then
        REPO=$(sed -n 's/^HIDWAY_REPO=//p' "$ETC/update.conf" 2>/dev/null | grep -v '<' || true)
        [[ -n $REPO ]] || REPO=$(origin_url "$here") || die "cannot tell the repository URL; pass --repo URL"
    fi

    if [[ -d $SRC/.git ]]; then
        say "checkout exists: ${SRC#"$ROOT"}"
    else
        say "cloning $REPO"
        as_build git clone --quiet "$REPO" "$SRC"
    fi

    say "configuration"
    if compgen -G "$ETC/*.conf" >/dev/null; then
        for f in "$ETC"/*.conf; do backup "$f"; done
    fi
    local overrides=("HIDWAY_REPO=$REPO")
    [[ $REF_SET == 1 ]] && overrides+=("HIDWAY_REF=$REF")
    put_conf "$here/hidwayd.conf.example" "$ETC/hidwayd.conf"
    put_conf "$here/update.conf.example" "$ETC/update.conf" "${overrides[@]}"

    # Build the first release and install the service files of that same
    # commit, both through the updater, so everything comes from one source.
    say "building the relay"
    HIDWAY_ROOT=$ROOT HIDWAY_BUILD_USER=$BUILD_USER bash "$here/hidway-update" --now
    say "service files"
    HIDWAY_ROOT=$ROOT HIDWAY_BUILD_USER=$BUILD_USER bash "$here/hidway-update" --apply-system

    "$SYSTEMCTL" enable --now hidway-update.timer
    prune_backups

    echo
    if configured; then
        "$SYSTEMCTL" enable hidwayd.service
        "$SYSTEMCTL" restart hidwayd.service
        say "done: hidwayd is running and follows '$(sed -n 's/^HIDWAY_REF=//p' "$ETC/update.conf")'"
    else
        say "installed. Next:"
        echo "    1. sudo nano /etc/hidway/hidwayd.conf     (replace the <placeholders>)"
        echo "    2. sudo systemctl enable --now hidwayd"
    fi
    echo "    status:    hidway-update --status"
    echo "    logs:      journalctl -u hidwayd -u hidway-update -f"
    echo "    backups:   /var/backups/hidway/"
}

main() {
    [[ $(id -u) == 0 || -n $ROOT ]] || die "run as root: sudo $0 $*"
    case ${1:-} in
    --refresh)
        [[ -d ${2:-} ]] || die "--refresh needs a directory"
        install_system_files "$2"
        prune_backups
        return
        ;;
    -h | --help)
        sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
        return
        ;;
    esac
    while (($#)); do
        case $1 in
        --ref) REF=${2:?--ref needs a value}; REF_SET=1; shift 2 ;;
        --repo) REPO=${2:?--repo needs a value}; REPO_SET=1; shift 2 ;;
        *) die "unknown option: $1 (see --help)" ;;
        esac
    done
    full_install
}

main "$@"
exit
