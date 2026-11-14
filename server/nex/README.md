# Strikers local NEX research server — incomplete

This is a **localhost protocol probe**, not a working online-play server.
It cannot issue NEX tickets, authenticate a game session, match two games or relay
Pia. Do not forward ports or deploy this build. Public and ZeroTier binds are
explicitly refused until those boundaries are implemented. The Ubuntu files are
preparation templates, not evidence of a deployment or Internet readiness.

Python was selected for portable bounded binary parsing, stdlib UDP/TLS support,
and an existing Python 3.13 environment. The server uses only the standard
library, supports Python 3.10+, and has no pip/npm install step. OpenSSL CLI is
needed only to create test certificates. Node and new packages are not needed.
All execution in this job used Windows loopback; Ubuntu execution is unverified.

## Run locally

Run from the project root. Windows uses `.venv/Scripts/python`; on Ubuntu use
`python3`. The same source and commands apply; the runtime client is Windows.

```text
python -m server.nex.generate_pki --out build/online/my-pki
python -m server.account_stub --key build/online/account.key --out build/online/player1.token --principal 1701
python -m server.account_stub --key build/online/account.key --out build/online/player2.token --principal 1702
```

Each command refuses to overwrite an existing credential/PKI directory. Change
`tls_certificate`/`tls_key` in a copy of `config.local.json` to the new PKI path.
The checked-in local config points to this job's `build/online/pki02` test PKI.

```text
python -m server.nex --config server/nex/config.local.json --log build/run/my-online-server.jsonl
python tools/audit/test_nex.py --out build/run/my-nex-tests
```

`--log -` writes JSONL to stdout, suitable for journald. `--seconds 30` bounds a
probe. SIGINT/SIGTERM close listeners. File logs are exclusive-create. Malformed
UDP packets are counted, not reflected; valid packets log bounded metadata,
never account tokens or RMC argument contents. Anonymous WebSocket RMC requests
get an explicit unsupported-method error, never successful authentication.

## Runtime configuration

The mode requires `STRIKERS_NETWORK=online`. Missing/invalid configuration leaves
network availability false. All destinations except the configured server IP
and explicit port allowlist remain blocked. This phase accepts only numeric
`127.0.0.1`, even if a public address is supplied. No hosts-file or certificate
store change is necessary.

| Environment variable | `strikers.ini` `[Network]` key | Value for this test |
| --- | --- | --- |
| `STRIKERS_ONLINE_SERVER` | `online_server` | `127.0.0.1` |
| `STRIKERS_ONLINE_PORTS` | `online_ports` | `10025,10026,10027,10028,443` |
| `STRIKERS_ONLINE_TOKEN_FILE` | `online_token_file` | Absolute path to that player's `.token` |
| `STRIKERS_ONLINE_CA_FILE` | `online_ca_file` | Absolute path to `ca.der` or `ca.pem` |
| `STRIKERS_ONLINE_OPENSSL` | `online_openssl_file` | Absolute path to existing `libssl-3.dll` |

Environment values override INI values. The runner selects the existing Python
installation's `DLLs/libssl-3.dll`, with `libcrypto-3.dll` alongside it. No DLL is
downloaded. The runtime loads that explicit local path, requires OpenSSL 3's
expected exports, and does not search PATH or the working directory. UNC and
remote-drive credentials/CA/library paths are rejected. A distributable client
will need a reviewed OpenSSL packaging/license/update arrangement.

Schannel was attempted first but `AcquireCredentialsHandle` failed with
`0x8009030e` in this environment. The tested backend is OpenSSL. It loads a fresh
trust store with **only this private CA**, verifies chain/date/server EKU/hostname,
uses TLS 1.2 or newer, and negotiates HTTP/1.1 ALPN. The guest cannot disable
verification. It does not load system/default roots or fetch CA URLs. Revocation
infrastructure is not provided by the development CA. The CA has a three-day
lifetime and leaf certificates two days; these are test credentials.

The original SDK NSD `%` environment placeholder expands to `lp1` only for the
recognized game name shapes. DNS never performs a real host lookup: it maps
those names to the configured loopback IP. Arbitrary hosts/suffix spoofs fail.
The names are protocol identifiers from the game; no Nintendo service is used.

## Isolated game probes

```text
python tools/audit/build_gameplay.py --opt --lan-sdk --online-main --output strikers_job17_online.exe
python tools/audit/run_online_pair.py --name my-online-pair --exe strikers_job17_online.exe --seconds 180 --seed-save build/run/job15-discovery02-userdata/save/strkrs.save
python tools/audit/review_captures.py build/run/my-online-pair/client1.log
python tools/audit/review_captures.py build/run/my-online-pair/client2.log
python tools/audit/test_online_replay.py build/run/my-online-pair/manifest.json
```

The default sequence attempts Quick Battle / Online Play, not a full match.
It is timing-sensitive: the saved probe reached different menus on the two
clients, so inspect both captures before claiming a particular menu path.
The final checker **must fail** for this probe server. A real match requires
server authentication, membership/Pia evidence, two independent inputs, a late
match-end save fence and reviewed matching results/return captures from both
clients. It does not accept unit tests as game evidence.

The runner creates separate userdata/config/token/recording/log files, records
owned process IDs and artifact hashes, and only terminates its own children.
Options include `--no-server`, `--restart-after SECONDS`, `--quit-client 1
--quit-after SECONDS`, `--pairs 2`, and custom `--client1-seq`/`--client2-seq`.
Mid-lobby/mid-match tests remain pending because no game lobby is reached yet.

## Wire coverage and missing work

Implemented primitives: strict 30-byte PRUDP v1 header and TLV framing,
12-byte PRUDPLite framing, bounded RMC request/response framing, an RC4 stream
primitive, sequence ordering/duplicate handling/u16 wrap, bounded fragment
assembly and zlib inflation. These primitives are not a complete transport:
packet signature derivation, real reliable retransmission/channels,
encryption/compression negotiation and authentication tickets are not integrated.

The game actually starts NEX with TLS/WebSocket on port 443. A UDP-only service
does not satisfy that path. The local WebSocket probe can establish anonymous
transport for investigation, logs decoded method IDs when available, and refuses
RMC success. Two real games reached LoginEx (protocol 10, method 6) over this
transport, but neither received a successful authentication response.
See `docs/phase7-online-local.md` for the binary protocol/method table and exact
remaining work. No NEX-compatible gathering schema or Pia relay is claimed.

`server/account_stub.py` exposes `LocalAccountProvider.issue/verify` and writes
the runtime token envelope. Tokens contain issuer, audience, nonzero u64
principal, issue/expiry times and random token ID; HS256 is verified by the local
provider. This provider is **not yet connected to NEX LoginEx**. Anyone with the
local operator key can issue development accounts. Job 18 must replace issuance
with the account service, define asymmetric server verification/key rotation,
and add real accounts/friends/invites. Do not publish the local signing key.

## Ubuntu / ZeroTier preparation

`config.public.example.json` and `config.zerotier.example.json` show address/port
configuration for the later host. They deliberately cannot start this incomplete
probe. `strikers-nex.service` is a localhost-only systemd template with journald
logging. `ufw-rules.txt` lists future rules; no rules have been applied.

Planned services are TCP 443 (observed NEX TLS/WebSocket), UDP 10025/10026
(auth/secure if that transport is selected), UDP 10027 (NAT check), and UDP 10028
(Pia relay). UDP assignments are proposed configurable ports, not recovered
Nintendo defaults. Do not forward them until wire services and authentication
are complete. A server behind home CGNAT cannot become publicly reachable through
port forwarding alone. The later ZeroTier option needs a reachable relay on the
overlay and validated client/server policy; it is not implemented here. Clients
behind CGNAT need an authenticated outbound relay path, which remains outstanding.

Create a source-only bundle without credentials, game assets, binaries or logs:

```text
python -m server.nex.bundle --out build/bundles/strikers-nex-probe.zip
```

No installation, service registration, firewall change, router change, remote
connection or deployment was performed by this job.
