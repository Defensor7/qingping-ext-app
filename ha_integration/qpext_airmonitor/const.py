"""Constants for the qpext_airmonitor integration."""
from __future__ import annotations

DOMAIN = "qpext_airmonitor"

# MQTT topic the device (qpext.so shim) listens on. {mac} is the wifi MAC
# without separators, uppercase — e.g. 582D3470A873.
TOPIC_TEMPLATE = "qpext/{mac}/dashboard/set"

# Config / options keys
CONF_MAC = "mac"
CONF_WIDGETS = "widgets"
CONF_EVENTS = "events"

# Widget types supported by the QML side (qpext/qml/Plugins/widgets/*.qml).
WIDGET_TYPES = [
    "sensor",
    "switch",
    "light",
    "climate",
    "media_player",
    "cover",
    "script",
    "scene",
    "button",
]

# Widget types that operate on a HA entity (vs. `button` which is service-only).
ENTITY_WIDGET_TYPES = [
    "sensor",
    "switch",
    "light",
    "climate",
    "media_player",
    "cover",
    "script",
    "scene",
]

# Map widget type → HA domain for filtering the entity selector.
DOMAIN_FOR_WIDGET_TYPE: dict[str, str] = {
    "sensor": "sensor",
    "switch": "switch",
    "light": "light",
    "climate": "climate",
    "media_player": "media_player",
    "cover": "cover",
    "script": "script",
    "scene": "scene",
}

# Default MDI icon per widget type — used when the user leaves the icon
# field empty. Keep these aligned with `MdiIcon.qml`'s known glyph set so
# the device side can resolve them without falling back to the help glyph.
DEFAULT_ICONS: dict[str, str] = {
    "sensor": "mdi:thermometer",
    "switch": "mdi:toggle-switch",
    "light": "mdi:lightbulb",
    "climate": "mdi:thermostat",
    "media_player": "mdi:television",
    "cover": "mdi:garage",
    "script": "mdi:script-text",
    "scene": "mdi:palette",
    "button": "mdi:gesture-tap-button",
}

# Visible tab names in MainPage.qml's PathView model. Used by the events
# (trigger) flow to populate the "switch_to" dropdown.
TAB_NAMES = [
    "airDatasView",
    "summaryView",
    "settingView",
    "appView",
    "qpextView",
    "qpextCamerasView",
]
