# Strikers relay

Internet rooms for Local Wireless. Games that are not on the same network find each other's rooms through this
server, and their Local Wireless traffic passes through it. Players set nothing up: no account, no port forwarding
and no VPN. They never see each other's IP addresses.

- One Python 3.8+ file, no packages, no database, nothing written to disk.
- One UDP port, 24890 by default.
- The protocol is described at the top of [strikers_relay.py](strikers_relay.py).
- The game's side is `runtime/services/relay_transport.c`.

## What it sees

The relay sees each player's IP address and the room's game traffic (the same data the game sends on a home
network). It does not keep either: no log line contains an IP address. With `-v` it logs counts, and when a room
opens or closes it logs only whether the room is public or uses a room key. A room key never reaches the server;
the game sends the first 16 bytes of its SHA-256 hash.

## Limits

Everything a game sends is checked:

- A game may send only to a player in the same room key and release, and only to one that has a room listed or has
  sent to it before.
- Room adverts are checked the way the game checks them.
- Answers to anything without a valid session are never bigger than the request.
- Per IP address: 16 sessions, 8 rooms, and 5 new sessions a second. Per player: 256 KB/s and 600 datagrams a
  second, and a room search returns up to 24 rooms.

A match uses about 10 KB/s per player.

## Run

```bash
python3 strikers_relay.py --port 24890 -v
```

Options:
- `--addr` (default `0.0.0.0`)
- `--max-sessions`
- `--sessions-per-ip`
- `--rooms-per-ip`
- `--bandwidth` (bytes a second per player)

## Install on a server

Any small Linux VPS with systemd and python3 works (Ubuntu 22.04 or newer, Debian 12). From this folder:

```bash
sh deploy.sh user@your-server
```

It copies the script to `/opt/strikers-relay`, installs `strikers-relay.service` (a dynamic user with no write
access) and starts it. If ufw is active, it also opens UDP 24890. Open that port in your provider's firewall too
if it has one.

Name it with a DNS record such as `relay.example.com`. On Cloudflare, use a grey-cloud ("DNS only") record, because
Cloudflare's proxy does not carry UDP. Then check it from any PC:

```bash
python3 relay_probe.py relay.example.com
```

## Point the game at it

The game's built-in relay is `relay.mtg9t4.fun` (`STRIKERS_RELAY_DEFAULT` in `runtime/services/relay_transport.c`): pointing
that name at a running relay switches internet rooms on for every player, with no new download. To use another relay,
each player sets `relay = relay.example.com` (or `host:port`) under `[Network]` in `strikers.ini`. Everyone who wants to
see each other's rooms uses the same relay.

## Test

```bash
python3 -m unittest -v test_strikers_relay
```

`tools/audit/test_relay.py` runs the game's real Local Wireless code in separate processes against a local relay.
