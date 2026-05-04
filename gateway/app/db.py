import os
import sqlite3
import threading
from typing import Optional

_DB_PATH = os.environ.get("GATEWAY_DB_PATH", "/feeding/gateway.db")
_lock = threading.Lock()
_conn: Optional[sqlite3.Connection] = None


def get_conn() -> sqlite3.Connection:
    global _conn
    if _conn is None:
        os.makedirs(os.path.dirname(_DB_PATH) or ".", exist_ok=True)
        _conn = sqlite3.connect(_DB_PATH, check_same_thread=False)
        _conn.row_factory = sqlite3.Row
        _conn.execute("PRAGMA journal_mode=WAL")
        _conn.execute("PRAGMA foreign_keys=ON")
    return _conn


def init() -> None:
    conn = get_conn()
    with _lock:
        conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS records (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                start_epoch INTEGER NOT NULL,
                stop_epoch INTEGER,
                volume_ml INTEGER,
                notes TEXT,
                device_id TEXT NOT NULL DEFAULT '',
                created_at INTEGER NOT NULL DEFAULT (CAST(strftime('%s','now') AS INTEGER))
            );
            CREATE INDEX IF NOT EXISTS idx_records_start ON records(start_epoch DESC);
            """
        )
        conn.commit()


def legacy_config_rows() -> dict:
    """Return rows from the old `config` table if it still exists, else {}.

    Used once on startup to seed config.json from a pre-JSON install.
    """
    conn = get_conn()
    with _lock:
        row = conn.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name='config'"
        ).fetchone()
        if not row:
            return {}
        rows = conn.execute("SELECT key, value FROM config").fetchall()
    return {r["key"]: r["value"] for r in rows}


def get_active() -> Optional[dict]:
    conn = get_conn()
    with _lock:
        row = conn.execute(
            "SELECT * FROM records WHERE stop_epoch IS NULL ORDER BY start_epoch DESC LIMIT 1"
        ).fetchone()
    return dict(row) if row else None


def stop_active(stop_epoch: int) -> bool:
    conn = get_conn()
    with _lock:
        cur = conn.execute(
            "UPDATE records SET stop_epoch=? "
            "WHERE id=(SELECT id FROM records WHERE stop_epoch IS NULL "
            "ORDER BY start_epoch DESC LIMIT 1)",
            (stop_epoch,),
        )
        conn.commit()
        return cur.rowcount > 0


def create_record(
    start_epoch: int,
    stop_epoch: Optional[int] = None,
    volume_ml: Optional[int] = None,
    notes: Optional[str] = None,
    device_id: str = "",
) -> int:
    conn = get_conn()
    with _lock:
        cur = conn.execute(
            "INSERT INTO records (start_epoch, stop_epoch, volume_ml, notes, device_id) "
            "VALUES (?, ?, ?, ?, ?)",
            (start_epoch, stop_epoch, volume_ml, notes, device_id),
        )
        conn.commit()
        return cur.lastrowid


def update_record(rid: int, **fields) -> None:
    allowed = {"start_epoch", "stop_epoch", "volume_ml", "notes", "device_id"}
    sets = []
    params: list = []
    for k, v in fields.items():
        if k not in allowed:
            continue
        sets.append(f"{k}=?")
        params.append(v)
    if not sets:
        return
    params.append(rid)
    conn = get_conn()
    with _lock:
        conn.execute(f"UPDATE records SET {', '.join(sets)} WHERE id=?", params)
        conn.commit()


def delete_record(rid: int) -> None:
    conn = get_conn()
    with _lock:
        conn.execute("DELETE FROM records WHERE id=?", (rid,))
        conn.commit()


def list_records(
    limit: Optional[int] = None,
    ids: Optional[list] = None,
    offset: int = 0,
) -> list:
    conn = get_conn()
    with _lock:
        if ids:
            qmarks = ",".join("?" * len(ids))
            rows = conn.execute(
                f"SELECT * FROM records WHERE id IN ({qmarks}) ORDER BY start_epoch DESC",
                ids,
            ).fetchall()
        elif limit is not None:
            rows = conn.execute(
                "SELECT * FROM records ORDER BY start_epoch DESC LIMIT ? OFFSET ?",
                (limit, offset),
            ).fetchall()
        else:
            rows = conn.execute(
                "SELECT * FROM records ORDER BY start_epoch DESC"
            ).fetchall()
    return [dict(r) for r in rows]


def count_records() -> int:
    conn = get_conn()
    with _lock:
        row = conn.execute("SELECT COUNT(*) AS n FROM records").fetchone()
    return int(row["n"]) if row else 0


