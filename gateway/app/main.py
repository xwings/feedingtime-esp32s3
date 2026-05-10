import asyncio
import os
import time
from contextlib import asynccontextmanager
from datetime import datetime
from pathlib import Path
from typing import Optional

from fastapi import (
    Depends,
    FastAPI,
    Form,
    Header,
    HTTPException,
    Request,
)
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from pydantic import BaseModel

from . import config, db, openclaw
from .util import zoneinfo

GATEWAY_TOKEN = os.environ.get("GATEWAY_TOKEN", "").strip()

BASE_DIR = Path(__file__).resolve().parent
templates = Jinja2Templates(directory=str(BASE_DIR / "templates"))


def filter_localtime(epoch: Optional[int], tz_name: str = "UTC") -> str:
    if not epoch:
        return ""
    return datetime.fromtimestamp(int(epoch), tz=zoneinfo(tz_name)).strftime(
        "%Y-%m-%d %H:%M"
    )


def filter_localdate_input(epoch: Optional[int], tz_name: str = "UTC") -> str:
    if not epoch:
        return ""
    return datetime.fromtimestamp(int(epoch), tz=zoneinfo(tz_name)).strftime("%Y-%m-%d")


def filter_localtime_only(epoch: Optional[int], tz_name: str = "UTC") -> str:
    if not epoch:
        return ""
    return datetime.fromtimestamp(int(epoch), tz=zoneinfo(tz_name)).strftime("%H:%M")


def filter_duration(start: Optional[int], stop: Optional[int]) -> str:
    if not start or not stop:
        return ""
    d = int(stop) - int(start)
    return f"{d // 60}m {d % 60}s"


templates.env.filters["localtime"] = filter_localtime
templates.env.filters["localdate_input"] = filter_localdate_input
templates.env.filters["localtime_only"] = filter_localtime_only
templates.env.filters["duration"] = filter_duration


def combine_date_time(date_str: str, time_str: str, tz_name: str = "UTC") -> Optional[int]:
    if not date_str or not time_str:
        return None
    date_str = date_str.strip()
    time_str = time_str.strip()
    if not date_str or not time_str:
        return None
    fmt = "%Y-%m-%dT%H:%M:%S" if time_str.count(":") >= 2 else "%Y-%m-%dT%H:%M"
    dt = datetime.strptime(f"{date_str}T{time_str}", fmt).replace(tzinfo=zoneinfo(tz_name))
    return int(dt.timestamp())


@asynccontextmanager
async def lifespan(app: FastAPI):
    db.init()
    config.migrate_from(db.legacy_config_rows)
    task = asyncio.create_task(openclaw.scheduler_loop())
    try:
        yield
    finally:
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass


app = FastAPI(title="OpenClaw Gateway", lifespan=lifespan)
app.mount("/static", StaticFiles(directory=str(BASE_DIR / "static")), name="static")


def check_token(authorization: Optional[str] = Header(None)) -> None:
    if not GATEWAY_TOKEN:
        return
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(status_code=401, detail="missing bearer token")
    token = authorization.split(" ", 1)[1].strip()
    if token != GATEWAY_TOKEN:
        raise HTTPException(status_code=403, detail="invalid token")


def state_payload() -> dict:
    return {
        "active": db.get_active(),
        "history": db.list_records(limit=8),
        "server_epoch": int(time.time()),
    }


# ---------------------------------------------------------------------------
# Device-facing API
# ---------------------------------------------------------------------------


class EventIn(BaseModel):
    type: str
    device_id: str = ""
    timestamp_epoch: Optional[int] = None


@app.post("/api/events", dependencies=[Depends(check_token)])
async def api_post_event(event: EventIn):
    if event.type not in ("start", "stop"):
        raise HTTPException(400, "type must be 'start' or 'stop'")
    ts = event.timestamp_epoch or int(time.time())
    if event.type == "start":
        active = db.get_active()
        if active is None:
            db.create_record(start_epoch=ts, device_id=event.device_id)
    else:
        db.stop_active(stop_epoch=ts)
    return state_payload()


@app.get("/api/state", dependencies=[Depends(check_token)])
async def api_get_state():
    return state_payload()


class SyncIn(BaseModel):
    record_ids: Optional[list[int]] = None


@app.post("/api/sync", dependencies=[Depends(check_token)])
async def api_post_sync(body: Optional[SyncIn] = None):
    rids = body.record_ids if body else None
    ok, msg = await openclaw.send_sync(rids)
    status = 200 if ok else 502
    return JSONResponse({"ok": ok, "message": msg}, status_code=status)


@app.get("/api/records", dependencies=[Depends(check_token)])
async def api_list_records(limit: int = 100):
    return db.list_records(limit=limit)


@app.get("/api/config", dependencies=[Depends(check_token)])
async def api_get_config():
    return config.load()


# ---------------------------------------------------------------------------
# Web UI
# ---------------------------------------------------------------------------


@app.get("/", response_class=HTMLResponse)
async def ui_home(
    request: Request,
    sync: Optional[str] = None,
    msg: Optional[str] = None,
    page: int = 1,
):
    cfg = config.load()
    try:
        dates_per_page = int(cfg.get("ui_show_count") or "10")
    except ValueError:
        dates_per_page = 10
    if dates_per_page < 1:
        dates_per_page = 10
    tz_name = cfg.get("timezone") or "UTC"
    tz = zoneinfo(tz_name)

    all_records = db.list_records()
    by_date: dict[str, list] = {}
    date_order: list[str] = []
    for r in all_records:
        d = datetime.fromtimestamp(int(r["start_epoch"]), tz=tz).strftime("%Y-%m-%d")
        if d not in by_date:
            by_date[d] = []
            date_order.append(d)
        by_date[d].append(r)
    total_dates = len(date_order)
    total_pages = max(1, (total_dates + dates_per_page - 1) // dates_per_page)
    if page < 1:
        page = 1
    if page > total_pages:
        page = total_pages
    start = (page - 1) * dates_per_page
    groups = [{"date": d, "records": by_date[d]} for d in date_order[start:start + dates_per_page]]

    now_epoch = int(time.time())
    auto_check_cutoff = now_epoch - 86400

    now = datetime.now(tz=tz)
    return templates.TemplateResponse(
        "index.html",
        {
            "request": request,
            "groups": groups,
            "active": db.get_active(),
            "config": cfg,
            "tz": tz_name,
            "now_date": now.strftime("%Y-%m-%d"),
            "now_time": now.strftime("%H:%M"),
            "page": page,
            "total_pages": total_pages,
            "total_records": len(all_records),
            "total_dates": total_dates,
            "dates_per_page": dates_per_page,
            "auto_check_cutoff": auto_check_cutoff,
            "config_keys_simple": [
                "openclaw_url",
                "webhook_token",
                "openclaw_agent_id",
                "openclaw_session_key",
                "openclaw_channel",
                "openclaw_to",
                "openclaw_name",
                "openclaw_wake_mode",
                "openclaw_deliver",
                "webhook_default_sync_count",
                "auto_sync_enabled",
                "auto_sync_hours",
                "timezone",
                "ui_show_count",
            ],
            "sync_result": sync,
            "sync_msg": msg or "",
        },
    )


@app.post("/records")
async def ui_create(
    date: str = Form(...),
    start_time: str = Form(...),
    stop_time: str = Form(""),
    volume_ml: str = Form(""),
    notes: str = Form(""),
    device_id: str = Form(""),
):
    cfg = config.load()
    tz = cfg.get("timezone") or "UTC"
    start_epoch = combine_date_time(date, start_time, tz)
    if start_epoch is None:
        raise HTTPException(400, "date and start_time required")
    stop_epoch = combine_date_time(date, stop_time, tz) if stop_time.strip() else None
    if stop_epoch is not None and stop_epoch < start_epoch:
        stop_epoch += 86400  # session crossed midnight
    db.create_record(
        start_epoch=start_epoch,
        stop_epoch=stop_epoch,
        volume_ml=int(volume_ml) if volume_ml.strip() else None,
        notes=notes or None,
        device_id=device_id or "",
    )
    return RedirectResponse("/", status_code=303)


@app.post("/records/save")
async def ui_bulk_save(request: Request):
    cfg = config.load()
    tz = cfg.get("timezone") or "UTC"
    form = await request.form()
    rids = [int(v) for v in form.getlist("record_id") if str(v).isdigit()]
    for rid in rids:
        date = (form.get(f"date_{rid}") or "").strip()
        start_time = (form.get(f"start_time_{rid}") or "").strip()
        stop_time = (form.get(f"stop_time_{rid}") or "").strip()
        if not date or not start_time:
            continue
        start_epoch = combine_date_time(date, start_time, tz)
        stop_epoch = combine_date_time(date, stop_time, tz) if stop_time else None
        if stop_epoch is not None and stop_epoch < start_epoch:
            stop_epoch += 86400  # session crossed midnight
        volume_ml = (form.get(f"volume_ml_{rid}") or "").strip()
        notes = form.get(f"notes_{rid}") or ""
        device_id = form.get(f"device_id_{rid}") or ""
        db.update_record(
            rid,
            start_epoch=start_epoch,
            stop_epoch=stop_epoch,
            volume_ml=int(volume_ml) if volume_ml else None,
            notes=notes or None,
            device_id=device_id or "",
        )
    return RedirectResponse("/", status_code=303)


@app.post("/records/delete")
async def ui_bulk_delete(request: Request):
    form = await request.form()
    rids = [int(v) for v in form.getlist("record_id") if str(v).isdigit()]
    for rid in rids:
        db.delete_record(rid)
    return RedirectResponse("/", status_code=303)


@app.post("/config")
async def ui_save_config(request: Request):
    form = await request.form()
    items = {k: str(v) for k, v in form.multi_items()}
    config.update(items)
    return RedirectResponse("/#config", status_code=303)


@app.post("/sync")
async def ui_sync(request: Request):
    form = await request.form()
    rids = [int(v) for v in form.getlist("record_id") if str(v).isdigit()]
    ok, msg = await openclaw.send_sync(rids or None)
    return RedirectResponse(
        f"/?sync={'ok' if ok else 'fail'}&msg={msg}", status_code=303
    )
