# Relay on the Raspberry Pi

`hidwayd` runs on the Raspberry Pi as a systemd service and keeps itself up to
date from the repository. This page covers installation, configuration,
updates, rollback and removal.

## Install

On the Pi (Raspberry Pi OS / Debian with systemd), from any clone of the
repository:

```bash
sudo apt install git build-essential
git clone <repository-url> ~/hidway && cd ~/hidway
sudo relay/deploy/install.sh            # follows the main branch
```

Then set your addresses and start the relay:

```bash
sudo nano /etc/hidway/hidwayd.conf      # replace the <placeholders>
sudo systemctl enable --now hidwayd
hidway-update --status
```

`install.sh` can be run again at any time. It never overwrites the files in
`/etc/hidway/`, and it backs up every file it replaces first.

Options: `--ref <branch>` or `--ref latest-tag` (see [Updates](#updates)), and
`--repo <url>` to follow a different repository (by default the origin of the
clone you run it from, converted to HTTPS).

## Layout

| Path | Contents |
|---|---|
| `/opt/hidway/src` | Checkout the Pi follows (owned by `hidway-build`) |
| `/opt/hidway/releases/<commit>/` | One directory per built release: `hidwayd` + `RELEASE` metadata |
| `/opt/hidway/current`, `previous` | Symlinks to the running and the previous release |
| `/etc/hidway/hidwayd.conf` | Relay settings (addresses, port, serial device) |
| `/etc/hidway/update.conf` | Updater settings (repository, branch or tags, releases kept) |
| `/etc/systemd/system/hidwayd.service` | The relay service |
| `/etc/systemd/system/hidway-update.{service,timer}` | Update check every 5 minutes |
| `/etc/udev/rules.d/99-hidway.rules` | `/dev/hidway-serial` for the Raspberry Pi Debug Probe |
| `/usr/local/sbin/hidway-update` | Update / rollback / status command |
| `/run/hidway/state` | Live state published by `hidwayd` (version, link, serial) |
| `/var/backups/hidway/<time>/` | Backups of replaced files and configuration (last 10 runs) |

Two system users are created: `hidway` runs the relay, `hidway-build` fetches
and compiles. Neither has a login shell.

## Configuration

`/etc/hidway/hidwayd.conf`:

| Setting | Meaning |
|---|---|
| `HIDWAY_BIND` | Tailscale address of the Pi; the relay listens on it only |
| `HIDWAY_ALLOW` | Tailscale address of the remote PC; every other source is dropped |
| `HIDWAY_PORT` | UDP port (default 47800) |
| `HIDWAY_SERIAL` | Serial device of the Pico link (default `/dev/hidway-serial`) |
| `HIDWAY_BAUD` | Serial speed (default 921600) |
| `HIDWAY_EXTRA_OPTS` | Extra options; remove `--quiet` for a per-second summary in the journal |

After a change: `sudo systemctl restart hidwayd`.

The udev rule covers the Raspberry Pi Debug Probe. For another USB-serial
adapter, point `HIDWAY_SERIAL` at its `/dev/serial/by-id/...` path and give the
`hidway` group access to it with a similar rule.

## Updates

`hidway-update.timer` checks the repository every 5 minutes. When a commit
changes the relay (`relay/`, `common/` or `tests/`):

1. The new commit is checked out and built by `hidway-build`, and the unit
   tests are run (`make check`). A failure stops here; nothing changes.
2. The binary is installed into its own `releases/<commit>/` directory.
3. If a session is live (link up), the switch waits for the next check after
   the session ends, so a game is never interrupted.
4. `current` is switched atomically and `hidwayd` restarted. If it does not
   come up healthy within 10 seconds, the previous release is restored
   automatically and the failed one is marked and not retried.
5. Old releases are pruned (`HIDWAY_KEEP`, default 3; current and previous are
   always kept).

What the Pi follows is set in `/etc/hidway/update.conf`:

- `HIDWAY_REF=main`: every push that touches the relay is deployed.
- `HIDWAY_REF=latest-tag`: only release tags `v*` are deployed.

### Service files

The relay runs unprivileged and sandboxed, so its binary is updated
automatically. The service files (systemd units, udev rule and
`hidway-update` itself) run as root, so they are never changed automatically.
When they change upstream, the updater logs it once and `--status` shows it.
Review the change and apply it with:

```bash
sudo hidway-update --apply-system
```

## Commands

```bash
hidway-update --status            # running version, link, releases
sudo hidway-update                # update now (waits if a session is live)
sudo hidway-update --now          # update even during a live session
sudo hidway-update --check        # is an update available? (exit 10 if yes)
sudo hidway-update --rollback     # back to the previous release
sudo hidway-update --apply-system # install changed service files
journalctl -u hidwayd -u hidway-update -f
```

## Uninstall

```bash
sudo relay/deploy/uninstall.sh            # remove services, udev rule and command
sudo relay/deploy/uninstall.sh --purge    # also /opt/hidway, /etc/hidway and the users
```

The configuration is backed up to `/var/backups/hidway/` first. Backups are
never removed.

## Security notes

- `hidwayd` runs as `hidway` with no capabilities, a read-only file system,
  access to serial devices only, and a system-call filter. It binds to the
  Tailscale address only, accepts a single peer, and caps input at 2000
  packets per second.
- Fetching and building run as `hidway-build`, never as root. Root only copies
  the built binary into place and switches the symlink.
- Following a branch means that whoever can push to it can change the relay
  binary on the Pi. Protect the repository account (two-factor
  authentication), or follow release tags (`HIDWAY_REF=latest-tag`).
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
