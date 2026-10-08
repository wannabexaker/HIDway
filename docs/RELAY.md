# Relay on the Raspberry Pi

`hidwayd` runs on the Raspberry Pi as a systemd service. Releases are deployed
deliberately (by tagging), checked once a day, installed only when no session
is live, and rolled back automatically if they do not come up healthy. This
page covers installation, status, releases, rollback and removal.

The Pi is assumed to be a shared server: the relay and its updater are
sandboxed and resource-bounded so they cannot disturb the Pi's other services.

## Install

On the Pi (Raspberry Pi OS / Debian with systemd), from any clone of the
repository:

```bash
sudo apt install git build-essential
git clone <repository-url> ~/hidway && cd ~/hidway
sudo relay/deploy/install.sh            # installs the newest release tag
```

Then set your addresses and start the relay:

```bash
sudo nano /etc/hidway/hidwayd.conf      # replace the <placeholders>
sudo systemctl enable --now hidwayd
hidway-status
```

`install.sh` can be run again at any time. It never overwrites the files in
`/etc/hidway/`, and it backs up every file it replaces first.

Options: `--ref main` to follow a branch instead of release tags (see
[Releases and updates](#releases-and-updates)), and `--repo <url>` to follow a
different repository (by default the origin of the clone you run it from,
converted to HTTPS).

## Status

```bash
hidway-status            # full report, no root needed
hidway-status --short    # one line; also shown at every SSH login
```

```
HIDway relay: OK

  relay     running v0.1.5, up 3h 12m
  session   idle (last input: 2026-10-06 22:41)
  serial    /dev/hidway-serial connected
  updates   following release tags; last check 2026-10-07 04:43: up-to-date - running v0.1.5
            next check: 2026-10-08 04:51 (now: sudo hidway-update)
  system    service files up to date
  releases
    * v0.1.5                   built 2026-10-06T10:20:54Z
    < v0.1.4                   built 2026-10-02T18:02:11Z
    (* current, < previous)
```

The first line is the overall state, also used as the exit code (handy for
monitoring):

| Level | Exit | Meaning |
|---|---|---|
| `OK` | 0 | Relay running, serial device connected, last update check fine |
| `WARN` | 1 | Running, but something needs attention: serial device missing, last update check failed, a release failed its health check, or service files to apply |
| `DOWN` | 2 | The relay is not running |

During a session the report shows the live packet rate, session id, sequence
gaps and rate-limited packets. Logs: `journalctl -u hidwayd -u hidway-update`.

## Releases and updates

A release is a tag `vX.Y.Z` on the repository. Pushes to `main` never reach
the Pi; deploying is a deliberate act:

```bash
git tag -a v0.2.0 -m "HIDway 0.2.0"
git push origin v0.2.0
# optional, to deploy right away instead of at the next daily check:
ssh <pi> sudo hidway-update
```

**Checks.** `hidway-update.timer` checks once a day at 04:30 (randomised by up
to 30 minutes; a check missed while the Pi was off runs at the next boot). A
check is a single small request (`git ls-remote`). Nothing is fetched or
built unless a newer release exists. Disable automatic checks with
`sudo systemctl disable --now hidway-update.timer`; `sudo hidway-update` still
works on demand.

**Deploying a new release:**

1. It is fetched and built by the unprivileged `hidway-build` user, and the
   unit tests are run (`make check`). A failure stops here; nothing changes.
2. The binary is installed into its own `releases/<commit>/` directory.
3. If a session is live, nothing is restarted: the release waits, and is
   installed at the next check after the session ends (or with
   `sudo hidway-update` when you are done).
4. `current` is switched atomically and `hidwayd` restarted. If it does not
   come up healthy within 10 seconds, the previous release is restored
   automatically.
5. Old releases are pruned (`HIDWAY_KEEP`, default 3; current and previous
   are always kept).

**Held releases.** A release that failed its health check, or that you rolled
back from, is held: it is not deployed again automatically. The next newer
release replaces it; `sudo hidway-update --now` retries it on purpose.

**Resource use.** Idle, `hidwayd` sleeps until there is work: no periodic
wakeups at all. The updater runs with idle CPU and I/O priority, at most
256 MB of memory and 15 minutes; the relay is limited to 32 MB and 4 tasks.

What the Pi follows is set in `/etc/hidway/update.conf`:

- `HIDWAY_REF=latest-tag` (default): the newest `vX.Y.Z` tag. Pre-release
  tags such as `v0.2.0-rc1` are ignored.
- `HIDWAY_REF=main` (or any branch): every commit that changes the relay
  (`relay/`, `common/`, `tests/`) is deployed. Useful only for development.

### Service files

The relay runs unprivileged and sandboxed, so its binary is deployed
automatically. The service files (systemd units, udev rule, login status line
and `hidway-update` itself) run as root, so they are never changed
automatically. When a release changes them, the updater logs it once and
`hidway-status` shows `WARN: service files to apply`. Review the change and
apply it with:

```bash
sudo hidway-update --apply-system
```

## Commands

```bash
hidway-status                     # health, session, serial, updates, releases
sudo hidway-update                # install the newest release (waits for an idle link)
sudo hidway-update --now          # also during a live session; retries a held release
sudo hidway-update --check        # is a newer release available? (exit 10 if yes)
sudo hidway-update --rollback     # back to the previous release, and stay there
sudo hidway-update --apply-system # install changed service files
journalctl -u hidwayd -u hidway-update -f
```

## Configuration

`/etc/hidway/hidwayd.conf`:

| Setting | Meaning |
|---|---|
| `HIDWAY_BIND` | Tailscale address of the Pi; the relay listens on it only |
| `HIDWAY_ALLOW` | Tailscale address of the remote PC; every other source is dropped |
| `HIDWAY_PORT` | UDP port (default 47800) |
| `HIDWAY_SERIAL` | Serial device of the Pico link (default `/dev/hidway-serial`) |
| `HIDWAY_BAUD` | Serial speed (default 921600) |
| `HIDWAY_EXTRA_OPTS` | Extra options; remove `--quiet` for a per-second summary in the journal during sessions |

After a change: `sudo systemctl restart hidwayd`.

### End-to-end encryption (optional)

Tailscale already encrypts the path end to end. If the traffic takes a path
that terminates encryption in the middle (for example Cloudflare WARP +
Tunnel), turn on HIDway's own encryption so that path only ever sees
ciphertext: every packet is sealed with XChaCha20-Poly1305 using a pre-shared
32-byte key, and replays are rejected by timestamp. Both clocks must be in
sync (NTP) within two minutes.

```bash
# on the Pi: create the key (readable by root and the service group only)
sudo sh -c 'umask 027; /opt/hidway/current/hidwayd --gen-key > /etc/hidway/hidwayd.key'
sudo chgrp hidway /etc/hidway/hidwayd.key
sudo cat /etc/hidway/hidwayd.key          # copy this value to the client
```

1. In `/etc/hidway/hidwayd.conf` set
   `HIDWAY_EXTRA_OPTS="--quiet --key-file /etc/hidway/hidwayd.key"` and
   restart the relay.
2. In the client's `hidway.ini` add `key = <the 64 hex characters>` and
   restart the client. The CONNECTION card shows a green **E2E** badge.
3. Check from the remote PC without arming anything:
   `hidway-probe --ini hidway.ini` (it reports `relay: encryption on`).

With a key, the relay accepts only sealed packets; without one it accepts only
plaintext. A mismatch is logged on the relay and shows as "no reply" on the
client. To turn encryption off again, remove `--key-file` and the `key` line.
Never commit or paste the key anywhere public; rotate it by generating a new
one on both ends.

The udev rule covers the Raspberry Pi Debug Probe. For another USB-serial
adapter, point `HIDWAY_SERIAL` at its `/dev/serial/by-id/...` path and give the
`hidway` group access to it with a similar rule.

## Layout

| Path | Contents |
|---|---|
| `/opt/hidway/src` | Checkout the releases are built from (owned by `hidway-build`) |
| `/opt/hidway/releases/<commit>/` | One directory per built release: `hidwayd` + `RELEASE` metadata |
| `/opt/hidway/current`, `previous` | Symlinks to the running and the previous release |
| `/opt/hidway/update-status` | Outcome of the last update check |
| `/etc/hidway/hidwayd.conf` | Relay settings (addresses, port, serial device) |
| `/etc/hidway/hidwayd.key` | End-to-end key, if encryption is on (`root:hidway`, mode 0640) |
| `/etc/hidway/update.conf` | Updater settings (repository, tags or branch, releases kept) |
| `/etc/systemd/system/hidwayd.service` | The relay service |
| `/etc/systemd/system/hidway-update.{service,timer}` | Daily release check |
| `/etc/udev/rules.d/99-hidway.rules` | `/dev/hidway-serial` for the Raspberry Pi Debug Probe |
| `/etc/update-motd.d/60-hidway` | Status line at login |
| `/usr/local/sbin/hidway-update` | Update / rollback command |
| `/usr/local/bin/hidway-status` | Status command (no root needed) |
| `/run/hidway/state` | Live state published by `hidwayd` |
| `/var/backups/hidway/<time>/` | Backups of replaced files and configuration (last 10 runs) |

Two system users are created: `hidway` runs the relay, `hidway-build` fetches
and compiles. Neither has a login shell.

## Uninstall

```bash
sudo relay/deploy/uninstall.sh            # remove services, udev rule, commands, login line
sudo relay/deploy/uninstall.sh --purge    # also /opt/hidway, /etc/hidway and the users
```

The configuration is backed up to `/var/backups/hidway/` first. Backups are
never removed.

## Security notes

- `hidwayd` runs as `hidway` with no capabilities, a read-only file system,
  access to serial devices only, and a system-call filter. It binds to the
  Tailscale address only, accepts a single peer, and caps input at 2000
  packets per second.
- Fetching, building and test runs happen as `hidway-build`, never as root.
  Root only copies the built binary into place and switches the symlink.
- Whoever can push a release tag can change the relay binary on the Pi.
  Protect the repository account (two-factor authentication) and add a
  GitHub tag ruleset for `v*` so release tags cannot be moved or deleted.
- Changes to root-run files always need `--apply-system`.

## Fail-safe behaviour

- The relay keeps running when the serial device disappears (probe unplugged,
  Pico reset), retries once per second, and starts every new connection with a
  RELEASE, so the Pico begins from "nothing held".
- No queue: while the serial line is still sending, a newer state replaces the
  pending one. A RELEASE discards anything pending and is always sent.
- A clean stop (update, rollback, `systemctl stop`) sends a RELEASE first. A
  crash cannot, so the Pico also releases everything on its own 250 ms link
  timeout.
