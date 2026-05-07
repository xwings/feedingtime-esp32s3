# Architecture

Two operating modes, picked at firmware build time via `GATEWAY_URL` in
`firmware/include/config.local.h` (empty = standalone, set = gateway).

## Standalone

```text
K2 press -> toggle feeding state -> render "Feeding now" / "Last fed" counter
NTP-synced clock when idle. In-RAM 8-slot ring buffer, cleared on reboot.
```

K1 short cycles clock ↔ history ↔ counter. K1 long is a no-op (no gateway
to sync to).

## Gateway

```text
ESP32 (any room)              Gateway (Docker)            OpenClaw
  K2 toggles feeding   -->    POST /api/events  -->       (none until sync)
  K1 long-press        -->    POST /api/sync    -->       POST /hooks/agent
  poll every ~3s       <--    GET  /api/state   (truth)
```

The gateway is the source of truth (SQLite, no upper limit on records — the
firmware only needs the newest 8 for the LCD). It enforces "at most one
active session globally": a `start` while one is already active is a no-op.

Each ESP32 keeps a 16-event in-RAM queue. If the gateway is unreachable when
K2 is pressed, the event queues and drains on the next successful poll. The
display reflects optimistic local state until the queue is drained, then
re-syncs from `/api/state`.

OpenClaw integration lives entirely in the gateway. The firmware never sees
OpenClaw URLs/tokens/templates — those are managed in the web UI and stored
in the gateway's `config` table.

## Schema

```sql
records (
  id INTEGER PRIMARY KEY,
  start_epoch INTEGER NOT NULL,
  stop_epoch INTEGER,         -- NULL while session is active
  volume_ml INTEGER,          -- entered via UI
  notes TEXT,
  device_id TEXT,             -- which ESP32 reported it
  created_at INTEGER
)
config (key TEXT PRIMARY KEY, value TEXT)
```

Times are UTC epoch ints; the UI renders them in the configured `timezone`
(IANA name). Config keys are listed in `gateway/README.md`.

## API

(Full details in `gateway/README.md`.)

- `POST /api/events` — `{type: "start"|"stop", device_id, timestamp_epoch?}`
- `GET  /api/state` — `{active, history (newest 8), server_epoch}`
- `POST /api/sync` — `{record_ids?: [..]}`

`Authorization: Bearer <token>` is required when the gateway env var
`GATEWAY_TOKEN` is set; LAN-trust by default.

## Time on the firmware

After Wi-Fi connects, NTP cycles through `cn.ntp.org.cn`, `ntp.ntsc.ac.cn`,
`cn.pool.ntp.org`, then falls back to `pool.ntp.org`, `time.cloudflare.com`,
`time.google.com`. Each server gets a 6-second window; the first one that
returns wins.

## Hardware quirk

LCD_CAM and I²C don't initialize cleanly in parallel on the DNESP32S3B:
`gfx->begin()` (LCD_CAM) must run *before* `Wire.begin()`, or the chip
hangs.

## Concurrency on the device

- Core 1 (Arduino main loop): button polling, drawing, UI tick.
- Core 0 (gateway task, gateway mode only): drains the pending event queue,
  fetches `/api/state`. On state change it sets `gatewayStateDirty`; the
  main loop redraws.
- A FreeRTOS mutex (`stateMutex`) guards `feedHistory`, `activeCounter`, and
  the pending event queue. Only the main loop draws (Arduino_GFX is not
  thread-safe); only the gateway task does HTTP.
