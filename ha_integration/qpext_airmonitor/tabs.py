"""Tab helpers: migration, id allocation, derived views (cameras list, button
discovery payloads). Kept in its own module to keep config_flow.py + __init__.py
focused on UI / lifecycle plumbing."""
from __future__ import annotations

import re
from typing import Any, Iterable

from .const import (
    CONF_CAMERAS,
    CONF_EVENTS,
    CONF_TABS,
    CONF_WIDGETS,
    DEFAULT_TAB_ICON,
    TAB_TYPE_CAMERA,
    TAB_TYPE_WIDGETS,
)


_ID_OK_RE = re.compile(r"[^a-z0-9_]+")


def slugify(name: str, fallback: str = "tab") -> str:
    """Turn a free-form name into an MQTT/QML-safe slug.

    Used both for tab ids (when one isn't already set) and for HA discovery
    button object_ids. Limited to lowercase alnum + underscore so it can
    safely appear in a topic path.
    """
    s = (name or "").strip().lower()
    s = _ID_OK_RE.sub("_", s).strip("_")
    return s[:32] or fallback


def unique_id(desired: str, taken: Iterable[str]) -> str:
    """Append a numeric suffix if `desired` clashes with anything in `taken`."""
    taken_set = set(taken)
    if desired not in taken_set:
        return desired
    base = desired
    n = 2
    while f"{base}_{n}" in taken_set:
        n += 1
    return f"{base}_{n}"


def migrate_options(opts: dict[str, Any]) -> dict[str, Any]:
    """Return a normalized options dict with a `tabs` list.

    Migration cases:
      - Options already have `tabs` → return as-is (with safe defaults filled in).
      - Options have legacy `widgets` → fold them into a single "Home Assistant"
        widgets-tab. Legacy `cameras` → one camera-tab each.
      - Empty → start with an empty `tabs` array.

    Legacy `widgets` / `cameras` arrays are left in the result so a downgraded
    integration can still load the entry; the new code path ignores them.
    """
    opts = dict(opts or {})
    tabs = list(opts.get(CONF_TABS) or [])

    if not tabs:
        legacy_w = list(opts.get(CONF_WIDGETS) or [])
        legacy_c = list(opts.get(CONF_CAMERAS) or [])
        taken: list[str] = []
        if legacy_w:
            tabs.append({
                "id": "ha",
                "name": "Home Assistant",
                "type": TAB_TYPE_WIDGETS,
                "widgets": legacy_w,
                "icon": "mdi:home-assistant",
            })
            taken.append("ha")
        for cam in legacy_c:
            base = slugify(cam.get("name") or cam.get("label") or "camera", "cam")
            tab_id = unique_id(base, taken)
            taken.append(tab_id)
            tabs.append({
                "id": tab_id,
                "name": cam.get("label") or cam.get("name") or "Camera",
                "type": TAB_TYPE_CAMERA,
                "camera": dict(cam),
                "icon": "mdi:cctv",
            })

    # Ensure every tab carries the keys the consumers expect.
    normalized: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    for t in tabs:
        if not isinstance(t, dict):
            continue
        ttype = t.get("type") or TAB_TYPE_WIDGETS
        if ttype not in (TAB_TYPE_WIDGETS, TAB_TYPE_CAMERA):
            ttype = TAB_TYPE_WIDGETS
        tid = t.get("id") or slugify(t.get("name") or "", "tab")
        tid = unique_id(tid, seen_ids)
        seen_ids.add(tid)
        entry: dict[str, Any] = {
            "id": tid,
            "name": t.get("name") or tid,
            "type": ttype,
            "icon": t.get("icon") or (
                "mdi:cctv" if ttype == TAB_TYPE_CAMERA else DEFAULT_TAB_ICON),
        }
        if ttype == TAB_TYPE_WIDGETS:
            entry["widgets"] = list(t.get("widgets") or [])
        else:
            entry["camera"] = dict(t.get("camera") or {})
        normalized.append(entry)

    return {
        CONF_TABS: normalized,
        CONF_EVENTS: list(opts.get(CONF_EVENTS) or []),
    }


def derive_cameras(tabs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Flat list of camera configs the shim's cam_thread can consume.

    Each entry carries `tab_id` so CamerasImpl.qml can pick its own camera.
    The shim writes the array verbatim to /data/qpext/cameras.json; nothing
    else parses the structure.
    """
    out: list[dict[str, Any]] = []
    for t in tabs:
        if t.get("type") != TAB_TYPE_CAMERA:
            continue
        cam = dict(t.get("camera") or {})
        cam["tab_id"] = t.get("id")
        out.append(cam)
    return out


def tab_qml_name(tab_id: str) -> str:
    """QML-side tab name used in MainPage.qml's PathView model and in
    `tab_event`/`switch_tab` MQTT messages. Single source of truth so the
    shim's hardcoded show buttons and the integration's per-tab discovery
    buttons agree."""
    return f"qpext_{tab_id}"


def tab_button_object_id(tab_id: str) -> str:
    """HA-discovery `object_id` for the per-tab nav button.

    Single-source naming so the integration can compute both the discovery
    topic (to publish) and the button's `uniq_id` (which HA uses to dedupe
    against the device).
    """
    return f"show_{tab_id}"
