#!/usr/bin/env bash
# Tests for relay/deploy/install.sh in a fake root: no root, no systemd, no
# network. Covers the settings options (--configure), the GPIO UART switch and
# the service-file refresh.
#
#   bash tests/deploy/test_install.sh      (needs bash, GNU coreutils/findutils)
set -uo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DEPLOY=$here/../../relay/deploy
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
pass=0
fail=0

check() { # check <name> <command...>
    local name=$1
    shift
    if "$@"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $name"
        sed 's/^/      /' "$work/out" 2>/dev/null | tail -n 5
    fi
}

setup() { # a fresh fake root with stub commands
    R=$work/root
    rm -rf "${R:?}" "${work:?}/bin"
    mkdir -p "$R/etc/hidway" "$R/boot/firmware" "$work/bin"
    # shellcheck disable=SC2016 # the stubs expand their own arguments
    printf '#!/bin/sh\necho "$*" >>"%s/systemctl.log"\n' "$work" >"$work/bin/systemctl"
    printf '#!/bin/sh\nexit 0\n' >"$work/bin/udevadm"
    # shellcheck disable=SC2016
    printf '#!/bin/sh\n[ "$1 $2" = "ip -4" ] && echo 100.64.1.2\n' >"$work/bin/tailscale"
    chmod +x "$work/bin/"*
    : >"$work/systemctl.log"
    printf 'Raspberry Pi 4 Model B Rev 1.5\0' >"$work/model"
    printf 'arm_64bit=1\n[all]\nenable_uart=1\n' >"$R/boot/firmware/config.txt"
}

run() { # run <install.sh args...>; output in $work/out
    HIDWAY_ROOT=$R HIDWAY_SYSTEMCTL=$work/bin/systemctl HIDWAY_UDEVADM=$work/bin/udevadm \
        HIDWAY_TAILSCALE=$work/bin/tailscale HIDWAY_MODEL_FILE=$work/model \
        bash "$DEPLOY/install.sh" "$@" >"$work/out" 2>&1
}

conf() { grep -E "^$1=" "$R/etc/hidway/hidwayd.conf" | tail -n 1 | cut -d= -f2-; }
count() { grep -cE "$1" "$2"; }
out_has() { grep -q -- "$1" "$work/out"; }
exists() { compgen -G "$1" >/dev/null; }
not() { ! "$@"; }

# --configure before anything is installed
setup
check "configure needs an installation" not run --configure --allow 100.64.9.9
check "  ...and says so" out_has "not installed yet"

# settings into an installed config
setup
cp "$DEPLOY/hidwayd.conf.example" "$R/etc/hidway/hidwayd.conf"
check "configure with settings" run --configure --bind auto --allow 100.64.9.9 \
    --wake-mac aa:bb:cc:dd:ee:ff --wake-to 10.0.0.1:9
check "  --bind auto asks Tailscale" test "$(conf HIDWAY_BIND)" = 100.64.1.2
check "  --allow" test "$(conf HIDWAY_ALLOW)" = 100.64.9.9
check "  optional --wake-mac is added" test "$(conf HIDWAY_WAKE_MAC)" = aa:bb:cc:dd:ee:ff
check "  optional --wake-to is added" test "$(conf HIDWAY_WAKE_TO)" = 10.0.0.1:9
check "  other settings kept" test "$(conf HIDWAY_PORT)" = 47800
check "  relay restarted once configured" grep -q "restart hidwayd.service" "$work/systemctl.log"
check "  previous config backed up" exists "$R/var/backups/hidway/*/etc/hidway/hidwayd.conf"

# the same again changes nothing and adds no duplicates
cp "$R/etc/hidway/hidwayd.conf" "$work/before"
check "configure again" run --configure --bind auto --allow 100.64.9.9 \
    --wake-mac aa:bb:cc:dd:ee:ff --wake-to 10.0.0.1:9
check "  idempotent" cmp -s "$work/before" "$R/etc/hidway/hidwayd.conf"
check "  one HIDWAY_WAKE_MAC line" test "$(count '^HIDWAY_WAKE_MAC=' "$R/etc/hidway/hidwayd.conf")" = 1

# bad values are refused and change nothing
cp "$R/etc/hidway/hidwayd.conf" "$work/before"
for bad in "--allow 1.2.3" "--allow 100.64.1.300" "--bind 10.0.0.256" "--wake-mac zz:bb:cc:dd:ee:ff" \
    "--wake-to 10.0.0.1:70000" "--wake-to host:9" "--serial /dev/x;reboot" "--uart wifi"; do
    # shellcheck disable=SC2086 # split the option from its value on purpose
    check "refuses $bad" not run --configure $bad
done
check "  config untouched after refusals" cmp -s "$work/before" "$R/etc/hidway/hidwayd.conf"
check "refuses an unknown option" not run --configure --colour red
check "refuses an option without a value" not run --configure --allow
check "  ...and says so" out_has "needs a value"

# GPIO UART on a Pi 4: kept in config.txt once, with a backup
check "--uart gpio on a Pi 4" run --configure --uart gpio
check "  dtoverlay=uart3 added" test "$(count '^dtoverlay=uart3$' "$R/boot/firmware/config.txt")" = 1
check "  config.txt backed up" exists "$R/var/backups/hidway/*/boot/firmware/config.txt"
check "--uart gpio again" run --configure --uart gpio
check "  still one dtoverlay=uart3" test "$(count '^dtoverlay=uart3$' "$R/boot/firmware/config.txt")" = 1

# GPIO UART elsewhere: refused before anything changes
setup
cp "$DEPLOY/hidwayd.conf.example" "$R/etc/hidway/hidwayd.conf"
printf 'Raspberry Pi 5 Model B Rev 1.0\0' >"$work/model"
cp "$R/boot/firmware/config.txt" "$work/cfg-before"
check "--uart gpio refused on a Pi 5" not run --configure --uart gpio --allow 100.64.9.9
check "  config.txt untouched" cmp -s "$work/cfg-before" "$R/boot/firmware/config.txt"
check "  hidwayd.conf untouched" test "$(conf HIDWAY_ALLOW)" = "<client-tailscale-ip>"

# service files
setup
check "--refresh installs the service files" run --refresh "$DEPLOY"
for f in usr/local/sbin/hidway-update usr/local/bin/hidway-wake etc/systemd/system/hidwayd.service \
    etc/udev/rules.d/99-hidway.rules; do
    check "  $f" test -f "$R/$f"
done
check "  hidway-wake is executable" test -x "$R/usr/local/bin/hidway-wake"
check "--refresh again changes nothing" run --refresh "$DEPLOY"
check "  quiet the second time" not out_has "installed"

echo "install.sh: $pass passed, $fail failed"
((fail == 0))
