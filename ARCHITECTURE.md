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

## Alarm

Triggers when the counter is in **Last fed** mode and elapsed seconds reach
`ALARM_MINUTES * 60`. Implementation lives in `firmware/src/main.cpp`:

- **One-shot per session.** Each session is keyed by its `start_epoch`
  (feeding) or `stop_epoch` (last fed). Once dismissed, the firing flag stays
  off until `activeCounter.sessionEpoch` changes — so gateway polls that
  re-apply the same state don't re-trigger.
- **View-independent trigger.** `tickAlarmCheck()` runs every loop pass
  regardless of the on-screen view; sitting on the clock view does not skip
  the trigger. When firing starts, the view auto-switches to the counter.
- **Audio path.** I2S0 master TX → ES8311 codec → speaker amp. The codec
  derives MCLK from BCLK (1.024 MHz at 16 kHz / 32-bit slots), avoiding the
  need for a separate MCLK pin. The amp's enable line sits on XL9555 P0.5,
  which is asserted in the same call that turns on the LCD backlight.
- **Silent mode.** With `ALARM_VOLUME 0`, the I2S/codec init is skipped
  entirely (`#if ALARM_VOLUME != 0`) and the PA amp stays off — no idle
  hum. The trigger logic still fires; only the speaker is silent.
- **Dismiss.** K1 short or long press, OR after `ALARM_DURATION_SEC`. K1
  while firing is consumed by the dismiss and does NOT cycle view or trigger
  sync.

## Audio pin map (Alientek DNESP32S3B / atk-dnesp32s3-box variant)

```text
I2S BCLK  = GPIO 21
I2S WS    = GPIO 13
I2S DOUT  = GPIO 14
I2S MCLK  = (unused — derived from BCLK)
Codec I²C = SDA GPIO 48 / SCL GPIO 45 (shared with XL9555)
Codec addr = 0x18 (ES8311)
PA enable = XL9555 P0.5 (high = amp on)
```

Confirmed against the xiaozhi-esp32 `atk-dnesp32s3-box` board config.

## Concurrency on the device

- Core 1 (Arduino main loop): button polling, drawing, UI tick.
- Core 0 (gateway task, gateway mode only): drains the pending event queue,
  fetches `/api/state`. On state change it sets `gatewayStateDirty`; the
  main loop redraws.
- A FreeRTOS mutex (`stateMutex`) guards `feedHistory`, `activeCounter`, and
  the pending event queue. Only the main loop draws (Arduino_GFX is not
  thread-safe); only the gateway task does HTTP.
