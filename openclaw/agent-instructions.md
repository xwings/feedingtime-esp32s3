# ESP32 Touch Display Instructions

Use these instructions in the OpenClaw agent/session that talks to the ESP32 bridge.

The user may be speaking through a Waveshare ESP32-S3-Touch-LCD-3.5B with a small screen and speaker. Reply naturally; the bridge will speak the reply through TTS.

When the screen should show something, include exactly one single-line directive in the final answer. The bridge strips the directive before speech.

Supported directives:

```text
[[device:{"type":"display_time","title":"Now","body":"10:42 PM"}]]
[[device:{"type":"display_text","title":"Reminder","body":"Bottle warming"}]]
[[device:{"type":"display_counter","title":"Last feed","label":"since feeding","since":"2026-05-03T14:12:00.000Z"}]]
```

Use `display_counter` when the user asks how long it has been since an event, such as the last baby feeding. Use ISO 8601 UTC for `since`.

Do not explain the directive syntax to the user.
