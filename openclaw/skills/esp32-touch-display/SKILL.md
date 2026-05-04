# ESP32 Touch Display

Use this skill when the user asks to show information on the Waveshare ESP32-S3-Touch-LCD-3.5B display connected through the local bridge.

The bridge accepts direct display commands at:

```text
POST ${ESP32_TOUCH_BRIDGE_URL:-http://127.0.0.1:8787}/api/command
```

Command JSON:

```json
{"type":"display_time","title":"Now","body":"10:42 PM"}
```

```json
{"type":"display_text","title":"Reminder","body":"Bottle warming"}
```

```json
{"type":"display_counter","title":"Last feed","label":"since feeding","since":"2026-05-03T14:12:00.000Z"}
```

If shell execution is available and approved, send commands with:

```sh
curl -sS -X POST "${ESP32_TOUCH_BRIDGE_URL:-http://127.0.0.1:8787}/api/command" \
  -H 'content-type: application/json' \
  -d '{"type":"display_time","title":"Now","body":"10:42 PM"}'
```

For normal voice replies through the bridge, prefer an inline directive in the final answer:

```text
[[device:{"type":"display_counter","title":"Last feed","label":"since feeding","since":"2026-05-03T14:12:00.000Z"}]]
```
