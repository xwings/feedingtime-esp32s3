import json
import os
import threading
from pathlib import Path
from typing import Callable, Optional

CONFIG_PATH = os.environ.get("GATEWAY_CONFIG_PATH", "/feeding/config.json")

DEFAULTS: dict = {
    "openclaw_url": "",
    "webhook_token": "",
    "openclaw_agent_id": "main",
    "openclaw_session_key": "",
    "openclaw_channel": "last",
    "openclaw_to": "",
    "openclaw_name": "ESP32 Baby Feeding",
    "openclaw_wake_mode": "now",
    "openclaw_deliver": "1",
    "webhook_default_sync_count": "8",
    "webhook_message_template": (
        "Use joplin-cli skill. Search for kinra notebook and update the note "
        "based on date and time. The following are the last feeding records. "
        "Append or update as needed.\n{records}"
    ),
    "webhook_record_format": (
        "{date} {start_time} -> {stop_time} ({duration}, {volume}ml) {notes}"
    ),
    "auto_sync_enabled": "0",
    "auto_sync_hours": "6",
    "auto_stop_minutes": "15",
    "timezone": "UTC",
    "ui_show_count": "10",
}

_lock = threading.Lock()
_cache: Optional[dict] = None


def _coerce(v) -> str:
    if isinstance(v, bool):
        return "1" if v else "0"
    return "" if v is None else str(v)


def _read_file() -> dict:
    p = Path(CONFIG_PATH)
    if not p.exists():
        return {}
    try:
        with p.open("r", encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, json.JSONDecodeError):
        return {}
    return data if isinstance(data, dict) else {}


def _write_file(data: dict) -> None:
    p = Path(CONFIG_PATH)
    p.parent.mkdir(parents=True, exist_ok=True)
    tmp = p.with_suffix(p.suffix + ".tmp")
    with tmp.open("w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, sort_keys=True, ensure_ascii=False)
        f.write("\n")
    os.replace(tmp, p)


def _merge(file_data: dict) -> dict:
    merged = {**DEFAULTS}
    for k, v in file_data.items():
        merged[k] = _coerce(v)
    return merged


def load() -> dict:
    global _cache
    with _lock:
        if _cache is None:
            file_data = _read_file()
            if not Path(CONFIG_PATH).exists():
                _write_file({**DEFAULTS})
            _cache = _merge(file_data)
        return dict(_cache)


def update(items: dict) -> dict:
    global _cache
    with _lock:
        current = _read_file()
        for k, v in items.items():
            current[k] = _coerce(v)
        _write_file(current)
        _cache = _merge(current)
        return dict(_cache)


def migrate_from(legacy_loader: Callable[[], dict]) -> None:
    if Path(CONFIG_PATH).exists():
        return
    rows = legacy_loader() or {}
    seed = {**DEFAULTS, **{k: _coerce(v) for k, v in rows.items()}}
    _write_file(seed)
