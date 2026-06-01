"""Constants for the qpext_airmonitor integration."""
from __future__ import annotations

DOMAIN = "qpext_airmonitor"

# MQTT topics the device (qpext.so shim) listens on. {mac} is the wifi MAC
# without separators, uppercase — e.g. 582D3470A873.
TOPIC_TEMPLATE         = "qpext/{mac}/dashboard/set"  # tabs + events
CAMERAS_TOPIC_TEMPLATE = "qpext/{mac}/cameras/set"    # camera array (derived from tabs)
# Per-endpoint weather payloads — the shim reads each retained message and
# writes it verbatim into /data/qpext/weather/<endpoint>.json, where its
# SSL_write/SSL_read hook substitutes it for the matching Qingping cloud
# `/daily/<endpoint>` response. See qpext/qpext.cpp::qpext_match_weather_endpoint.
WEATHER_TOPIC_TEMPLATE = "qpext/{mac}/weather/{endpoint}/set"
WEATHER_ENDPOINTS = ("now", "locate", "weatherNow", "dailyForecasts", "hourlyForecasts")

# HA MQTT-discovery topic prefix for the per-tab navigation buttons that the
# integration publishes itself (one button entity per user-defined tab).
DISCOVERY_BUTTON_TOPIC = "homeassistant/button/{node}/{object_id}/config"

# Config / options keys
CONF_MAC = "mac"
CONF_TABS = "tabs"           # new (multi-tab schema)
CONF_WIDGETS = "widgets"     # legacy (migrated to tabs on load; kept for fallback)
CONF_EVENTS = "events"
CONF_CAMERAS = "cameras"     # legacy (migrated to tabs on load)
CONF_WEATHER = "weather"     # weather-feeder config (entity selections + city name)

# Sub-keys inside options[CONF_WEATHER].
CONF_WEATHER_ENTITY    = "weather_entity"   # weather.* — drives temp + skycon + forecast
CONF_AQI_ENTITY        = "aqi_entity"       # sensor.* — current AQI for ИКВ display
CONF_UV_ENTITY         = "uv_entity"        # sensor.* — current UV index for УФИ display
CONF_HUMIDITY_ENTITY   = "humidity_entity"  # sensor.* (optional) — override humidity
CONF_CITY_NAME         = "city_name"        # display name shown on the weather widget

# Home Assistant `weather` entity condition strings → Qingping `skycon` enum
# values the stock app understands (renders icons for). HA conditions list
# is documented at https://www.home-assistant.io/integrations/weather/.
# Day-vs-night is decided at publish time based on the local sun.
HA_CONDITION_TO_SKYCON_DAY = {
    "sunny":           "CLEAR_DAY",
    "clear-night":     "CLEAR_DAY",         # day-context fallback
    "cloudy":          "CLOUDY",
    "partlycloudy":    "PARTLY_CLOUDY_DAY",
    "rainy":           "RAIN",
    "pouring":         "RAIN",
    "lightning":       "RAIN",
    "lightning-rainy": "RAIN",
    "snowy":           "SNOW",
    "snowy-rainy":     "SNOW",
    "hail":            "SNOW",
    "fog":             "FOG",
    "windy":           "WIND",
    "windy-variant":   "WIND",
    "exceptional":     "CLOUDY",
}
HA_CONDITION_TO_SKYCON_NIGHT = {
    "sunny":           "CLEAR_NIGHT",
    "clear-night":     "CLEAR_NIGHT",
    "cloudy":          "CLOUDY",
    "partlycloudy":    "PARTLY_CLOUDY_NIGHT",
    "rainy":           "RAIN",
    "pouring":         "RAIN",
    "lightning":       "RAIN",
    "lightning-rainy": "RAIN",
    "snowy":           "SNOW",
    "snowy-rainy":     "SNOW",
    "hail":            "SNOW",
    "fog":             "FOG",
    "windy":           "WIND",
    "windy-variant":   "WIND",
    "exceptional":     "CLOUDY",
}
SKYCON_FALLBACK = "CLOUDY"
DEFAULT_CITY_NAME = "Home"

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
