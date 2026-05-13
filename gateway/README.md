# OpenClaw Feeding Gateway

Tiny FastAPI + SQLite service that lets multiple ESP32 baby-feeding trackers
share state and forwards records to OpenClaw on demand or on a schedule.

## Run

```sh
docker compose up -d --build
```

Then open http://localhost:8080/ — records, manual edit, and OpenClaw config
all live on one page.

Persisted in `./feeding/` on the host (mounted to `/feeding` in the container):

- `gateway.db` — SQLite, holds feeding records.
- `config.json` — OpenClaw settings, message/record templates, sync schedule,
  timezone, UI options. Editable from the web UI **or** by hand; written
  atomically. Keys match those in the Config section below.

Change the host path or host port in `docker-compose.yml` if you need different
bindings. If you're upgrading from a build that stored config in SQLite, the
gateway copies the old `config` table into `config.json` on first start.

## Auth

By default, set `GATEWAY_TOKEN` to `""` in `docker-compose.yml` (LAN-trust, no
auth). Set a non-empty token to require `Authorization: Bearer <token>` on all
`/api/*` endpoints (and the same token in firmware `config.local.h`).

## API

Device-facing:

- `POST /api/events` — `{type: "start"|"stop", device_id, timestamp_epoch?}`
  starts a session (no-op if one is already active) or stops the active one.
  Returns the new state payload.
- `GET /api/state` — returns `{active, history (last 8), server_epoch}`.
- `POST /api/sync` — body `{record_ids?: [..]}`. Posts the chosen records (or
  the newest `webhook_default_sync_count` if omitted) to OpenClaw.

UI/admin:

- `GET /` — web UI: records table with inline edit, add-record form, OpenClaw
  config form, "Sync selected" button.
- `POST /records`, `POST /records/{id}/edit`, `POST /records/{id}/delete` —
  form actions.
- `POST /config` — saves the form.
- `POST /sync` — same as `/api/sync` but redirects back to the UI.

## Auto-sync

Set `auto_sync_enabled = 1` and `auto_sync_hours = 6` in the Config form.
The scheduler checks once a minute and posts the newest
`webhook_default_sync_count` records every N hours.

## Config keys

| Key | Default | Notes |
| --- | --- | --- |
| `openclaw_url` | "" | e.g. `http://host:18789/hooks/agent` |
| `webhook_token` | "" | bearer token for the webhook |
| `openclaw_agent_id` | `main` | |
| `openclaw_session_key` | "" | optional |
| `openclaw_channel` | `last` | `last` or `discord` |
| `openclaw_to` | "" | required when channel = `discord` |
| `openclaw_name` | `ESP32 Baby Feeding` | |
| `openclaw_wake_mode` | `now` | |
| `openclaw_deliver` | `1` | `1`/`0` |
| `webhook_default_sync_count` | `8` | how many newest records to send |
| `webhook_message_template` | (multi-line) | use `{records}` placeholder |
| `webhook_record_format` | `{date} {start_time} -> {stop_time} ({duration}, {volume}ml) {notes}` | per-line template |
| `auto_sync_enabled` | `0` | `1` to enable scheduler |
| `auto_sync_hours` | `6` | interval in hours |
| `auto_stop_minutes` | `15` | auto-stop an active session after this many minutes (0 disables) |
| `default_volume_ml` | `` | pre-fills the ml field of the Add-record form |
| `default_device_id` | `` | pre-fills the Device field of the Add-record form |
| `timezone` | `UTC` | IANA name, e.g. `Asia/Shanghai` |
| `ui_show_count` | `10` | dates per page on the web UI (records grouped by date; rows from the last 24h are pre-checked for upload) |
