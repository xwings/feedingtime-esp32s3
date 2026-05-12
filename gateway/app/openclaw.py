import asyncio
import time
from datetime import datetime
from typing import Optional

import httpx

from . import config, db
from .util import zoneinfo

_last_auto_sync_ts: float = 0.0


def render_message(records: list, cfg: dict) -> str:
    template = cfg.get("webhook_message_template") or "{records}"
    rec_format = cfg.get("webhook_record_format") or "{date} {start_time} -> {stop_time}"
    tzinfo = zoneinfo(cfg.get("timezone") or "UTC")

    lines = []
    for r in records:
        start = datetime.fromtimestamp(r["start_epoch"], tz=tzinfo)
        stop_epoch = r.get("stop_epoch")
        if stop_epoch:
            stop = datetime.fromtimestamp(stop_epoch, tz=tzinfo)
            stop_s = stop.strftime("%H:%M:%S")
            dur = stop_epoch - r["start_epoch"]
            dur_s = f"{dur // 60}m {dur % 60}s"
        else:
            stop_s = "..."
            dur_s = "active"

        vol = r.get("volume_ml")
        vol_s = str(vol) if vol is not None else ""
        notes = r.get("notes") or ""

        line = rec_format.format(
            date=start.strftime("%Y-%m-%d"),
            start_time=start.strftime("%H:%M:%S"),
            stop_time=stop_s,
            duration=dur_s,
            volume=vol_s,
            notes=notes,
            device_id=r.get("device_id") or "",
        )
        # Tidy up artifacts when optional placeholders are empty.
        line = (
            line.replace("(, )", "")
            .replace(", )", ")")
            .replace("(, ", "(")
            .replace("()", "")
            .rstrip()
        )
        lines.append(line)

    rendered = "\n".join(lines)
    if "{records}" in template:
        return template.replace("{records}", rendered)
    return template + ("\n" + rendered if rendered else "")


async def send_sync(record_ids: Optional[list] = None) -> tuple[bool, str]:
    cfg = config.load()
    url = (cfg.get("openclaw_url") or "").strip()
    token = (cfg.get("webhook_token") or "").strip()
    if not url or not token:
        return False, "Webhook URL or token not configured"

    if record_ids:
        records = db.list_records(ids=record_ids)
    else:
        try:
            n = int(cfg.get("webhook_default_sync_count") or "8")
        except ValueError:
            n = 8
        records = db.list_records(limit=n)

    if not records:
        return False, "No records to sync"

    message = render_message(records, cfg)
    body = {
        "message": message,
        "name": cfg.get("openclaw_name") or "",
        "wakeMode": cfg.get("openclaw_wake_mode") or "now",
        "deliver": (cfg.get("openclaw_deliver") or "1").lower() in ("1", "true", "yes"),
    }
    for json_key, cfg_key in [
        ("agentId", "openclaw_agent_id"),
        ("sessionKey", "openclaw_session_key"),
        ("channel", "openclaw_channel"),
        ("to", "openclaw_to"),
    ]:
        v = (cfg.get(cfg_key) or "").strip()
        if v:
            body[json_key] = v

    headers = {
        "Authorization": f"Bearer {token}",
        "Content-Type": "application/json",
    }

    try:
        async with httpx.AsyncClient(timeout=15.0) as client:
            resp = await client.post(url, json=body, headers=headers)
        if 200 <= resp.status_code < 300:
            return True, f"Posted {len(records)} record(s)"
        return False, f"HTTP {resp.status_code}: {resp.text[:200]}"
    except httpx.TimeoutException:
        return False, "Timeout"
    except Exception as e:
        return False, f"{type(e).__name__}: {e}"


def _enforce_auto_stop(cfg: dict) -> None:
    try:
        minutes = int(cfg.get("auto_stop_minutes") or "15")
    except ValueError:
        minutes = 15
    if minutes <= 0:
        return
    active = db.get_active()
    if not active:
        return
    cap = int(active["start_epoch"]) + minutes * 60
    if int(time.time()) >= cap:
        if db.stop_active(stop_epoch=cap):
            print(f"[scheduler] auto-stopped session {active['id']} at {minutes}min cap")


async def scheduler_loop() -> None:
    """Periodic auto-sync + auto-stop loop. Cancellable."""
    global _last_auto_sync_ts
    try:
        while True:
            await asyncio.sleep(60)
            try:
                cfg = config.load()
                _enforce_auto_stop(cfg)
                if (cfg.get("auto_sync_enabled") or "0").lower() not in ("1", "true", "yes"):
                    continue
                try:
                    hours = int(cfg.get("auto_sync_hours") or "6")
                except ValueError:
                    hours = 6
                if hours <= 0:
                    continue
                now = time.time()
                if _last_auto_sync_ts and now - _last_auto_sync_ts < hours * 3600:
                    continue
                ok, msg = await send_sync()
                if ok:
                    _last_auto_sync_ts = now
                    print(f"[scheduler] auto-sync ok: {msg}")
                else:
                    print(f"[scheduler] auto-sync skipped: {msg}")
            except Exception as e:
                print(f"[scheduler] error: {e}")
    except asyncio.CancelledError:
        pass
