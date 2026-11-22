# Strikers report service

Players send a problem report only when they choose to: **F1 → Report → Send report** in the game, or **Yes** when
the launcher asks after a crash. The game's side is `runtime/core/report_send.c`. This Worker stores the reports for
90 days, and `tools/reports/reports.py` reads them. It runs on Cloudflare's free plan, using Workers and KV only.

## What is stored

Each report is one zip. It holds:
- the player's description;
- the game's logs;
- system.txt (processor, Windows build, memory);
- strikers.ini and controllers.ini, with the invite code, room key and group service removed;
- optionally a screenshot, and a crash's `.txt` and minidump.

The game takes the Windows account name and the PC name out of every text file before zipping. The minidump is
binary and is sent only after the player said yes to a question that names it.

The Worker never stores or logs IP addresses. An address is used only for the rate limit (2 reports a minute). Every
report expires after 90 days.

## What it costs: nothing

The service is built to stay inside Cloudflare's free allowances whichever plan the account is on:
- **KV writes:** a report takes 2 writes (the zip and the month's usage record). `DAILY_CAP` (300) keeps a day under
  the 1,000 free writes.
- **KV storage:** `STORE_CAP_MB` (900) keeps everything stored under the 1 GB free. Each month's usage record counts
  the bytes stored that month, and a delete gives its space back.
- **Requests:** the free plan answers 100,000 requests a day and then returns errors; it never bills. Workers Paid
  includes 10 million a month.

When a cap is reached, players get "The report service is full" and the report stays on their PC. Nothing is billed.
On the free plan Cloudflare has no way to bill: past a limit it refuses requests instead.

## Upload format

`POST /v1/report`, `Content-Type: application/zip`, body = the zip (22 bytes to 10 MiB, starting with `PK\x03\x04`).

Headers, percent-encoded UTF-8:

| Header | Limit | Required |
|---|---|---|
| `X-Strikers-Version` | 32 | yes |
| `X-Strikers-Kind` | `menu`, `f9` or `crash` | yes |
| `X-Strikers-GPU` | 128 | no |
| `X-Strikers-CPU` | 128 | no |
| `X-Strikers-OS` | 64 | no |
| `X-Strikers-Memory` | 16 | no |
| `X-Strikers-Description` | 1000 (cut, not refused) | no |

The answer is `{"id":"R-XXXXXX"}`, or `{"error":"..."}` in words the game shows to the player as they are:
- 400: not a zip, or a bad header;
- 413: too big;
- 429: the rate limit;
- 503: the daily cap, the storage cap, or the service is down.

## Admin

Every admin call needs `Authorization: Bearer <ADMIN_TOKEN>`:
- `GET /v1/admin/reports?cursor=` lists the reports with their summary;
- `GET /v1/admin/report?key=r/...` downloads one zip;
- `DELETE /v1/admin/report?key=r/...` deletes one.

## Deploy

From this folder (`npm install` first, or link `node_modules` to `../group/node_modules`):

```bash
npx wrangler kv namespace create REPORTS
```

Put the printed id in `wrangler.toml`, then:

```bash
npx wrangler secret put ADMIN_TOKEN
npx wrangler deploy
```

The game's built-in address is `STRIKERS_REPORT_URL_DEFAULT` in `runtime/core/report_send.c`. A player can point
somewhere else with `report_url` under `[Network]` in `strikers.ini`, or turn sending off with `report_url = off`.

## Read the reports

Store the address and token once:
- `%APPDATA%\strikers-reports\url`
- `%APPDATA%\strikers-reports\token`

Then:

```bash
python -I tools/reports/reports.py list
python -I tools/reports/reports.py fetch
python -I tools/reports/reports.py summary
```

`fetch` downloads into `build/reports/`, which git ignores. `summary` groups the reports by version, GPU vendor and
kind, and lists the signs found in each log: crash exceptions, audio gaps, the frame rate, missing NVN functions and
lost devices.

## Test

```bash
npm test
```

This runs the Worker in workerd (miniflare) with a local KV and rate limit and no outbound fetches.
