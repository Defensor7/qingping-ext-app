"""Airmonitor App Extension — Home Assistant integration.

Publishes the dashboard widget composition (chosen via the options UI) to the
device's MQTT topic `qpext/<mac>/dashboard/set`. The qpext.so shim on the
device subscribes to that topic and atomically merges the payload into
`/data/qpext/widgets.json` (preserving the local `ha.token`); QML hot-reloads
within ~1.5 s.
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
    CONF_EVENTS,
    CONF_MAC,
    CONF_WIDGETS,
    DOMAIN,
    TOPIC_TEMPLATE,
)

_LOGGER = logging.getLogger(__name__)


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Set up an Airmonitor App Extension from a config entry."""
    hass.data.setdefault(DOMAIN, {})[entry.entry_id] = entry

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

    # Publish current state on (re)load.
    await _publish_dashboard(hass, entry)

    # Re-publish whenever the options change.
    entry.async_on_unload(entry.add_update_listener(_async_update_listener))

    # Once per HA boot: register a service so users can re-emit the retained
    # MQTT message without opening the options UI (e.g. after the device was
    # flashed/wiped and its widgets.json is empty).
    if not hass.services.has_service(DOMAIN, "republish"):
        async def _service_republish(call: ServiceCall) -> None:
            for ent in hass.data.get(DOMAIN, {}).values():
                await _publish_dashboard(hass, ent)
        hass.services.async_register(
            DOMAIN,
            "republish",
            _service_republish,
            schema=vol.Schema({}),
        )

    return True


async def async_unload_entry(hass: HomeAssistant, entry: ConfigEntry) -> bool:
    """Unload a config entry."""
    data = hass.data.get(DOMAIN, {})
    data.pop(entry.entry_id, None)
    if not data:
        hass.services.async_remove(DOMAIN, "republish")
    return True


async def _async_update_listener(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Forward options changes to the device."""
    await _publish_dashboard(hass, entry)


async def _publish_dashboard(hass: HomeAssistant, entry: ConfigEntry) -> None:
    """Build the widgets+events JSON and publish it retained to MQTT."""
    mac = entry.data[CONF_MAC]
    topic = TOPIC_TEMPLATE.format(mac=mac)
    payload = {
        CONF_WIDGETS: list(entry.options.get(CONF_WIDGETS, [])),
        CONF_EVENTS: list(entry.options.get(CONF_EVENTS, [])),
    }
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
        "qpext_airmonitor: published %d widgets, %d events to %s (%d bytes)",
        len(payload[CONF_WIDGETS]),
        len(payload[CONF_EVENTS]),
        topic,
        len(payload_str),
    )


def _format_mac(mac: str) -> str:
    """Format a bare MAC (no separators) as xx:xx:xx:xx:xx:xx for HA's device registry."""
    s = mac.replace(":", "").replace("-", "").upper()
    if len(s) != 12:
        return mac
    return ":".join(s[i : i + 2] for i in range(0, 12, 2))
