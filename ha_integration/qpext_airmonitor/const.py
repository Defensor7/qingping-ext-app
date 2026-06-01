"""Constants for the qpext_airmonitor integration."""
from __future__ import annotations

DOMAIN = "qpext_airmonitor"

# MQTT topics the device (qpext.so shim) listens on. {mac} is the wifi MAC
# without separators, uppercase — e.g. 582D3470A873.
TOPIC_TEMPLATE         = "qpext/{mac}/dashboard/set"  # tabs + events
CAMERAS_TOPIC_TEMPLATE = "qpext/{mac}/cameras/set"    # camera array (derived from tabs)

# HA MQTT-discovery topic prefix for the per-tab navigation buttons that the
# integration publishes itself (one button entity per user-defined tab).
DISCOVERY_BUTTON_TOPIC = "homeassistant/button/{node}/{object_id}/config"

# Config / options keys
CONF_MAC = "mac"
CONF_TABS = "tabs"           # new (multi-tab schema)
CONF_WIDGETS = "widgets"     # legacy (migrated to tabs on load; kept for fallback)
CONF_EVENTS = "events"
CONF_CAMERAS = "cameras"     # legacy (migrated to tabs on load)

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

# Tab types
TAB_TYPE_WIDGETS = "widgets"
TAB_TYPE_CAMERA = "camera"
TAB_TYPES = [TAB_TYPE_WIDGETS, TAB_TYPE_CAMERA]

# Default icon shown next to each tab's "Show <name>" HA button. The user
# can override per-tab via the options flow.
DEFAULT_TAB_ICON = "mdi:tab"

# Stock-app tabs that exist regardless of qpext config. Used by the
# triggers (events) flow to populate the "switch_to" dropdown alongside
# user-defined tabs.
STOCK_TAB_NAMES = [
    "airDatasView",
    "summaryView",
    "settingView",
    "appView",
]
