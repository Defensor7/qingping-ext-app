"""Airmonitor App Extension — Home Assistant integration.

Publishes the device's tab composition (chosen via the options UI) to MQTT.
Each (re-)publish hits three retained topics:

  - `qpext/<mac>/dashboard/set` → full {tabs, events} payload. The shim
    writes it verbatim to /data/qpext/widgets.json; the QML side reads
    tabs[] and dynamically rebuilds the PathView model.
  - `qpext/<mac>/cameras/set`   → flat array of camera configs derived
    from camera-type tabs. The shim's cam_thread consumes this.
  - `homeassistant/button/qpext_<mac>/show_<tab.id>/config` → one MQTT-
    discovery button per user tab, so HA exposes a "Show <tab name>" entity
    that switches the device to that tab via the cmd topic.

When a tab is removed an empty retained payload is published to its old
discovery topic so HA unregisters the button.
"""
from __future__ import annotations

import json
import logging
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry
from homeassistant.core import HomeAssistant, ServiceCall
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import device_registry as dr

from .const import (
    CAMERAS_TOPIC_TEMPLATE,
    CONF_CAMERAS,
    CONF_EVENTS,
    CONF_MAC,
    CONF_TABS,
    CONF_WIDGETS,
    DEFAULT_TAB_ICON,
    DISCOVERY_BUTTON_TOPIC,
    DOMAIN,
    TOPIC_TEMPLATE,
)
from .tabs import (
    derive_cameras,
    migrate_options,
    tab_button_object_id,
    tab_qml_name,
)

_LOGGER = logging.getLogger(__name__)


def _device_id(mac: str) -> str:
    """MAC-normalized device id mirroring qpext_mqtt.cpp's `dev_id` so the
    discovery payloads we publish bind to the same HA device as the shim's
    own discovery messages (sensors, reboot button, …)."""
    norm = mac.replace(":", "").replace("-", "").upper()
    return f"qpext_{norm}"


def _device_block(mac: str) -> dict[str, Any]:
    """Mirror of qpext_mqtt.cpp's `dev_blk` so HA merges our buttons with the
    shim-side discovery entries."""
    return {
        "identifiers": [_device_id(mac)],
        "name": "Airmonitor App Extension",
        "manufacturer": "Qingping",
        "model": "Air Monitor 2",
        "connections": [["mac", _format_mac(mac)]],
    }


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up an Airmonitor App Extension from a config entry."""
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = entry
    # Per-entry book-keeping: the set of tab ids whose discovery button we
    # have already published. On options update we diff against this set to
    # know which buttons to unpublish (empty retained payload).
    bookkeeping = hass.data.setdefault(f"{DOMAIN}_state", {}).setdefault(
        entry.entry_id, {"published_button_ids": set()})

    mac = entry.data[CONF_MAC]
    device_registry = dr.async_get(hass)
    device_registry.async_get_or_create(
        config_entry_id=entry.entry_id,
        identifiers={(DOMAIN, mac)},
        connections={(dr.CONNECTION_NETWORK_MAC, _format_mac(mac))},
        name=f"Airmonitor App Extension {mac}",
        manufacturer="Qingping",
        model="Air Monitor 2",
        sw_version="qpext",
    )

    # Migrate legacy options on first load. We DON'T persist the migration
    # immediately — that would write to options during async_setup_entry,
    # which can race with HA's own load. Instead the OptionsFlow.__init__
    # does the migration and writes on first save; the publish path below
    # reads the migrated view via migrate_options() each time so the device
    # always gets the new schema even before the user opens the UI.
    await _publish_all(hass, entry, bookkeeping)

    # Re-publish whenever the options change.
    entry.async_on_unload(entry.add_update_listener(_async_update_listener))

    # Once per HA boot: register a service so users can re-emit the retained
    # MQTT message without opening the options UI (e.g. after the device was
    # flashed/wiped and its widgets.json is empty).
    if not hass.services.has_service(DOMAIN, "republish"):
        async def _service_republish(call: ServiceCall) -> None:
            for ent in hass.data.get(DOMAIN, {}).values():
                book = hass.data[f"{DOMAIN}_state"].get(ent.entry_id, {
                    "published_button_ids": set()})
                await _publish_all(hass, ent, book)
        hass.services.async_register(
            DOMAIN,
            "republish",
            _service_republish,
            schema=vol.Schema({}),
        )

    # Programmatic seed: write options onto an existing config entry. Used by
    # dev/bootstrap_ha.py to inject a demo widget of every supported type
    # without driving the options UI. Accepts either the new `tabs` key or
    # the legacy `widgets`/`cameras` keys (which are run through the same
    # migration as a regular options-flow save).
    if not hass.services.has_service(DOMAIN, "set_options"):
        async def _service_set_options(call: ServiceCall) -> None:
            entry_id = call.data.get("entry_id")
            entries = hass.config_entries.async_entries(DOMAIN)
            target = None
            if entry_id:
                target = next((e for e in entries if e.entry_id == entry_id), None)
            elif entries:
                target = entries[0]
            if target is None:
                _LOGGER.warning("set_options: no qpext_airmonitor entries")
                return
            merged = dict(target.options or {})
            # If the caller passed only legacy `widgets`/`cameras` (no `tabs`)
            # they want a fresh seed from those — drop any existing tabs so
            # migrate_options() rebuilds them from the legacy lists. Otherwise
            # the existing tabs would shadow the legacy data and the seed
            # would silently no-op.
            if CONF_TABS not in call.data and (
                CONF_WIDGETS in call.data or CONF_CAMERAS in call.data
            ):
                merged.pop(CONF_TABS, None)
            for key in (CONF_TABS, CONF_WIDGETS, CONF_EVENTS, CONF_CAMERAS):
                if key in call.data:
                    merged[key] = list(call.data[key]) if call.data[key] else []
            # Normalize through the same migration the UI uses.
            normalized = migrate_options(merged)
            hass.config_entries.async_update_entry(target, options=normalized)
            _LOGGER.info("set_options: applied to %s (%d tabs)",
                         target.entry_id, len(normalized[CONF_TABS]))
        hass.services.async_register(
            DOMAIN,
            "set_options",
            _service_set_options,
            schema=vol.Schema({
                vol.Optional("entry_id"): str,
                vol.Optional(CONF_TABS): list,
                vol.Optional(CONF_WIDGETS): list,
                vol.Optional(CONF_EVENTS): list,
                vol.Optional(CONF_CAMERAS): list,
            }),
        )

    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    data = hass.data.get(DOMAIN, {})
    data.pop(entry.entry_id, None)
    hass.data.get(f"{DOMAIN}_state", {}).pop(entry.entry_id, None)
    if not data:
        hass.services.async_remove(DOMAIN, "republish")
        hass.services.async_remove(DOMAIN, "set_options")
    return True


async def _async_update_listener(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Forward options changes to the device."""
    book = hass.data[f"{DOMAIN}_state"].setdefault(
        entry.entry_id, {"published_button_ids": set()})
    await _publish_all(hass, entry, book)


async def _publish_all(
    hass: HomeAssistant, entry: ConfigEntry, bookkeeping: dict[str, Any]
) -> None:
    """Push the full state to MQTT: dashboard + cameras + per-tab buttons."""
    normalized = migrate_options(entry.options or {})
    tabs: list[dict[str, Any]] = normalized[CONF_TABS]
    events: list[dict[str, Any]] = normalized[CONF_EVENTS]
    await _publish_dashboard(hass, entry, tabs, events)
    await _publish_cameras(hass, entry, tabs)
    await _publish_tab_buttons(hass, entry, tabs, bookkeeping)


async def _publish_dashboard(
    hass: HomeAssistant,
    entry: ConfigEntry,
    tabs: list[dict[str, Any]],
    events: list[dict[str, Any]],
) -> None:
    """Build the {tabs, events} JSON and publish it retained to MQTT."""
    mac = entry.data[CONF_MAC]
    topic = TOPIC_TEMPLATE.format(mac=mac)
    payload = {CONF_TABS: tabs, CONF_EVENTS: events}
    payload_str = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    try:
        await hass.services.async_call(
            "mqtt",
            "publish",
            {"topic": topic, "payload": payload_str, "retain": True},
            blocking=True,
        )
    except HomeAssistantError as err:
        _LOGGER.error("qpext_airmonitor: MQTT publish failed: %s", err)
        return
    _LOGGER.info(
        "qpext_airmonitor: published %d tabs, %d events to %s (%d bytes)",
        len(tabs), len(events), topic, len(payload_str),
    )


async def _publish_cameras(
    hass: HomeAssistant, entry: ConfigEntry, tabs: list[dict[str, Any]]
) -> None:
    """Publish the cameras list retained to MQTT — shim writes it to
    /data/qpext/cameras.json, cam_thread restarts pipelines. The QML
    Cameras tab filters this array by `tab_id` to pick its own camera."""
    mac = entry.data[CONF_MAC]
    topic = CAMERAS_TOPIC_TEMPLATE.format(mac=mac)
    cameras = derive_cameras(tabs)
    payload = {CONF_CAMERAS: cameras}
    payload_str = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    try:
        await hass.services.async_call(
            "mqtt",
            "publish",
            {"topic": topic, "payload": payload_str, "retain": True},
            blocking=True,
        )
    except HomeAssistantError as err:
        _LOGGER.error("qpext_airmonitor: cameras publish failed: %s", err)
        return
    _LOGGER.info(
        "qpext_airmonitor: published %d cameras to %s (%d bytes)",
        len(cameras), topic, len(payload_str),
    )


# Buttons published by previous releases of the shim that the multi-tab
# rewrite obsoleted. We sweep them on the first publish per HA boot so HA
# stops showing stale "Show camera" / "Show HA dashboard" entries that point
# at tab names that no longer exist in the QML model. Empty retained payload
# tells HA to forget the entity. Idempotent — once the broker drops them
# the empty re-publish is a no-op.
LEGACY_TAB_BUTTON_IDS = ("show_camera",)


async def _publish_tab_buttons(
    hass: HomeAssistant,
    entry: ConfigEntry,
    tabs: list[dict[str, Any]],
    bookkeeping: dict[str, Any],
) -> None:
    """One MQTT-discovery button per user tab. Pressing it sends a
    `switch_tab` payload on the device's cmd topic.

    Unpublishes (empty retained payload) any tab id we previously published
    that no longer exists, so removed tabs don't leave dangling button
    entities in HA.
    """
    mac = entry.data[CONF_MAC]
    dev_id = _device_id(mac)
    cmd_topic = f"qpext/{mac.replace(':', '').replace('-', '').upper()}/cmd"
    dev_blk = _device_block(mac)

    current_ids = {t["id"] for t in tabs}
    previous_ids: set[str] = set(bookkeeping.get("published_button_ids") or set())

    # One-shot sweep of legacy static buttons the previous shim release used
    # to publish. Empty retained payload tells HA to forget the entity. After
    # the first sweep per HA boot the broker has no retained value so further
    # empties are no-ops; the flag avoids spamming the publish on each
    # options change.
    if not bookkeeping.get("legacy_swept"):
        for legacy in LEGACY_TAB_BUTTON_IDS:
            topic = DISCOVERY_BUTTON_TOPIC.format(node=dev_id, object_id=legacy)
            try:
                await hass.services.async_call(
                    "mqtt", "publish",
                    {"topic": topic, "payload": "", "retain": True},
                    blocking=True,
                )
            except HomeAssistantError:
                pass
        bookkeeping["legacy_swept"] = True

    # Unpublish removed.
    for tid in previous_ids - current_ids:
        topic = DISCOVERY_BUTTON_TOPIC.format(
            node=dev_id, object_id=tab_button_object_id(tid))
        try:
            await hass.services.async_call(
                "mqtt", "publish",
                {"topic": topic, "payload": "", "retain": True},
                blocking=True,
            )
            _LOGGER.info("qpext_airmonitor: unpublished tab button %s", tid)
        except HomeAssistantError as err:
            _LOGGER.error("qpext_airmonitor: unpublish %s failed: %s", topic, err)

    # (Re-)publish current.
    for tab in tabs:
        tid = tab["id"]
        topic = DISCOVERY_BUTTON_TOPIC.format(
            node=dev_id, object_id=tab_button_object_id(tid))
        press_payload = json.dumps(
            {"action": "switch_tab", "name": tab_qml_name(tid)},
            ensure_ascii=False, separators=(",", ":"))
        cfg = {
            "name": f"Show {tab.get('name') or tid}",
            "uniq_id": f"{dev_id}_show_{tid}",
            "obj_id": f"{dev_id}_show_{tid}",
            "command_topic": cmd_topic,
            "payload_press": press_payload,
            "icon": tab.get("icon") or DEFAULT_TAB_ICON,
            "device": dev_blk,
        }
        try:
            await hass.services.async_call(
                "mqtt", "publish",
                {
                    "topic": topic,
                    "payload": json.dumps(cfg, ensure_ascii=False,
                                          separators=(",", ":")),
                    "retain": True,
                },
                blocking=True,
            )
        except HomeAssistantError as err:
            _LOGGER.error("qpext_airmonitor: tab button %s publish failed: %s",
                          tid, err)

    bookkeeping["published_button_ids"] = current_ids
    _LOGGER.info("qpext_airmonitor: tab buttons synced (%d active, %d removed)",
                 len(current_ids), len(previous_ids - current_ids))


def _format_mac(mac: str) -> str:
    """Format a bare MAC (no separators) as xx:xx:xx:xx:xx:xx for HA's device registry."""
    s = mac.replace(":", "").replace("-", "").upper()
    if len(s) != 12:
        return mac
    return ":".join(s[i : i + 2] for i in range(0, 12, 2))
