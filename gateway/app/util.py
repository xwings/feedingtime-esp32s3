from datetime import timezone


def zoneinfo(tz_name: str):
    """Resolve an IANA timezone name, falling back to UTC if zoneinfo is
    unavailable (e.g. base image without tzdata)."""
    try:
        from zoneinfo import ZoneInfo
        return ZoneInfo(tz_name)
    except Exception:
        return timezone.utc
