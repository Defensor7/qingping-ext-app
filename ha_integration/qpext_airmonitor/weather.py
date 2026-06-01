"""Weather-feeder: turn the user's HA `weather.*` entity (plus optional
AQI / UV sensors) into the per-endpoint JSON payloads our shim's SSL
hook substitutes for Qingping cloud's `/daily/*` responses.

Each call to `publish_weather()` re-derives ALL five endpoint payloads
from current HA state and re-publishes them retained to
`qpext/<mac>/weather/<endpoint>/set`. The shim writes them verbatim to
`/data/qpext/weather/<endpoint>.json`; the next time the stock app polls
the matching cloud URL the SSL hook hands back our synthetic response
instead of letting the request out.

We don't try to invent fields the cloud's data has and HA's data doesn't:
empty defaults (humidity=0, aqi=0, ultraviolet=0, pm_*=0, …) match what
the Qingping cloud actually returns in regions where those metrics are
unavailable — the stock UI tolerates it gracefully.
"""
from __future__ import annotations

import json
import logging
import time as time_mod
from datetime import datetime, timezone
from typing import Any

from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.sun import is_up

from .const import (
    CONF_AQI_ENTITY,
    CONF_CITY_NAME,
    CONF_HUMIDITY_ENTITY,
    CONF_MAC,
    CONF_UV_ENTITY,
    CONF_WEATHER_ENTITY,
    DEFAULT_CITY_NAME,
    HA_CONDITION_TO_SKYCON_DAY,
    HA_CONDITION_TO_SKYCON_NIGHT,
    SKYCON_FALLBACK,
    WEATHER_TOPIC_TEMPLATE,
)

_LOGGER = logging.getLogger(__name__)


# --------------------------------------------------------------------------- #
# Small helpers                                                               #
# --------------------------------------------------------------------------- #

def _state_float(hass: HomeAssistant, entity_id: str | None,
                 default: float = 0.0) -> float:
    """Read a numeric sensor state safely. Returns `default` when the entity
    is missing, unavailable or non-numeric."""
    if not entity_id:
        return default
    s = hass.states.get(entity_id)
    if s is None or s.state in ("unknown", "unavailable", "", None):
        return default
    try:
        return float(s.state)
    except (TypeError, ValueError):
        return default


def _skycon_for(condition: str, hass: HomeAssistant) -> str:
    """Map HA `weather` condition string → Qingping skycon enum value,
    using the day/night variant based on the local sun position."""
    table = (HA_CONDITION_TO_SKYCON_DAY if is_up(hass)
             else HA_CONDITION_TO_SKYCON_NIGHT)
    return table.get(condition, SKYCON_FALLBACK)


def _bearing_to_dir(bearing: float | int | None) -> str:
    """16-point compass label (N/NNE/NE/.../NNW). Used in the wind block
    that the stock weather widget displays under the temperature."""
    if bearing is None:
        return "N"
    try:
        b = float(bearing) % 360.0
    except (TypeError, ValueError):
        return "N"
    points = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
              "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"]
    return points[int((b + 11.25) // 22.5) % 16]


def _wind_level(speed_kmh: float) -> int:
    """Beaufort scale (rough). Stock app shows the integer level as a
    bar-segment count under the wind direction arrow."""
    if speed_kmh < 1:   return 0
    if speed_kmh < 6:   return 1
    if speed_kmh < 12:  return 2
    if speed_kmh < 20:  return 3
    if speed_kmh < 29:  return 4
    if speed_kmh < 39:  return 5
    if speed_kmh < 50:  return 6
    if speed_kmh < 62:  return 7
    if speed_kmh < 75:  return 8
    return 9


# --------------------------------------------------------------------------- #
# Endpoint payload builders                                                   #
# --------------------------------------------------------------------------- #

def _build_now() -> dict[str, Any]:
    """`/daily/now` payload: current server time. Stock app uses this to
    sync its internal clock against the cloud — we just hand it
    HA-local time."""
    now = datetime.now()
    return {
        "time": now.strftime("%Y-%m-%d %H:%M:%S"),
        "timestamp": int(now.timestamp()),
    }


def _build_locate(cfg: dict[str, Any], hass: HomeAssistant) -> dict[str, Any]:
    """`/daily/locate` payload: city / timezone / coordinate metadata.
    Stock app caches this once at startup; the city name flows through to
    the small "Moscow" / "<city>" label at the top of the weather widget."""
    name = cfg.get(CONF_CITY_NAME) or DEFAULT_CITY_NAME
    home = hass.config
    return {
        "city_id":         "local",
        "name":            name,
        "name_en":         name,
        "name_cn":         name,
        "name_cn_tw":      name,
        "country":         "Local",
        "country_en":      "Local",
        "country_cn":      "Local",
        "country_cn_tw":   "Local",
        "area_cn_second":  name,
        "timezone":        str(home.time_zone),
        "timezone_gmt":    datetime.now().astimezone().strftime("GMT%z").replace(
                              "GMT-0", "GMT-").replace("GMT+0", "GMT+"),
        "coordinate": {
            "longitude": str(home.longitude),
            "latitude":  str(home.latitude),
        },
    }


def _build_weather_now(cfg: dict[str, Any], hass: HomeAssistant) -> dict[str, Any]:
    """`/daily/weatherNow` payload: the snapshot the stock app uses for the
    main weather widget — current temp, skycon icon, wind, ИКВ (aqi), УФИ
    (ultraviolet)."""
    name = cfg.get(CONF_CITY_NAME) or DEFAULT_CITY_NAME
    weather_id = cfg.get(CONF_WEATHER_ENTITY)
    w = hass.states.get(weather_id) if weather_id else None
    cond = (w.state if w else "cloudy")
    attrs = (w.attributes if w else {}) or {}

    temperature = float(attrs.get("temperature") or 0)
    # HA delivers humidity as a 0-100 percent; the cloud schema is 0-1.
    humidity_pct = attrs.get("humidity")
    if humidity_pct is None and cfg.get(CONF_HUMIDITY_ENTITY):
        humidity_pct = _state_float(hass, cfg[CONF_HUMIDITY_ENTITY], 0)
    humidity = (float(humidity_pct) / 100.0) if humidity_pct is not None else 0
    wind_speed = float(attrs.get("wind_speed") or 0)
    wind_dir = _bearing_to_dir(attrs.get("wind_bearing"))

    aqi = int(_state_float(hass, cfg.get(CONF_AQI_ENTITY), 0))
    uv  = int(_state_float(hass, cfg.get(CONF_UV_ENTITY), 0))

    return {
        "city_id": "local",
        "city": {
            "cityId":     "local",
            "country":    "Local",
            "province":   "",
            "city":       name,
            "longitude":  str(hass.config.longitude),
            "latitude":   str(hass.config.latitude),
            "timezone":   str(hass.config.time_zone),
            "timezoneFmt": datetime.now().astimezone().strftime("UTC%z"),
            "cnCity":     name,
            "cnAddress":  {"cityId":"local","country":"Local",
                           "province":name,"city":name},
            "enAddress":  {"cityId":"local","country":"Local",
                           "province":"","city":name},
            "name":       name,
            "name_cn":    name,
            "name_en":    name,
            "name_cn_tw": name,
        },
        "weather": {
            "pub_time":         int(time_mod.time()),
            "skycon":           _skycon_for(cond, hass),
            "humidity":         humidity,
            "temp_max":         temperature,
            "temp_min":         temperature,
            "temperature":      temperature,
            "probability":      0,
            "wind": {
                "wind_dir":     wind_dir,
                "wind_level":   _wind_level(wind_speed),
                "speed":        wind_speed,
            },
            "aqi":              aqi,
            "aqi_us":           aqi,
            "aqi_day_max_cn":   0,
            "aqi_day_min_cn":   0,
            "aqi_day_max_en":   0,
            "aqi_day_min_en":   0,
            "o3":               0,
            "so2":              0,
            "no2":              0,
            "co":               0,
            "pm_25":            0,
            "pm_10":            0,
            "ultraviolet":      uv,
            "vehicle_limit":    {"type": "city_unlimited"},
            "noAqi":            False,
            "so2_us":           0,
            "no2_us":           0,
            "o3_us":            0,
            "co_us":            0,
        },
    }


def _daily_entry(f: dict[str, Any], hass: HomeAssistant) -> dict[str, Any]:
    """Translate one HA daily-forecast row into the cloud's daily
    dailyForecasts schema."""
    dt = f.get("datetime", "")
    # HA's datetime is ISO-8601 like "2026-06-01T00:00:00+00:00"; clip to date.
    date = dt.split("T", 1)[0] if dt else datetime.now().strftime("%Y-%m-%d")
    try:
        ts = int(datetime.fromisoformat(dt.replace("Z", "+00:00")).timestamp())
    except (ValueError, AttributeError):
        ts = int(time_mod.time())
    cond = f.get("condition", "cloudy")
    skycon = HA_CONDITION_TO_SKYCON_DAY.get(cond, SKYCON_FALLBACK)
    skycon_night = HA_CONDITION_TO_SKYCON_NIGHT.get(cond, SKYCON_FALLBACK)
    t_high = float(f.get("temperature") or 0)        # HA uses `temperature` for daily-high
    t_low  = float(f.get("templow") or t_high)
    t_avg  = (t_high + t_low) / 2.0
    humidity_pct = f.get("humidity") or 0
    humidity = float(humidity_pct) / 100.0
    wind_speed = float(f.get("wind_speed") or 0)
    return {
        "date":      date,
        "timestamp": ts,
        "skycon": {
            "datetime": date,
            "value":    skycon,
            "day":      skycon,
            "night":    skycon_night,
        },
        "wind": {
            "datetime":   date,
            "speed":      wind_speed,
            "max":        wind_speed,
            "min":        wind_speed,
            "avg":        wind_speed,
            "wind_dir":   3,    # the cloud uses an enum here; 3 ≈ light
            "wind_level": _wind_level(wind_speed),
        },
        "temperature": {
            "date":      date,
            "timestamp": ts,
            "max":       t_high,
            "min":       t_low,
            "avg":       t_avg,
        },
        "humidity": {
            "date":      date,
            "timestamp": ts,
            "max":       humidity,
            "min":       humidity,
            "avg":       humidity,
        },
        "pm25": {"date": date, "max": 0, "min": 0, "avg": 0},
    }


def _hourly_entry(f: dict[str, Any]) -> dict[str, Any]:
    """Translate one HA hourly-forecast row into the cloud's hourly
    hourlyForecasts schema."""
    dt = f.get("datetime", "")
    try:
        ts = int(datetime.fromisoformat(dt.replace("Z", "+00:00")).timestamp())
    except (ValueError, AttributeError):
        ts = int(time_mod.time())
    label = dt[:16].replace("T", " ") if dt else ""
    cond = f.get("condition", "cloudy")
    skycon = HA_CONDITION_TO_SKYCON_DAY.get(cond, SKYCON_FALLBACK)
    temperature = float(f.get("temperature") or 0)
    humidity_pct = f.get("humidity") or 0
    humidity = float(humidity_pct) / 100.0
    wind_speed = float(f.get("wind_speed") or 0)
    wind_dir = _bearing_to_dir(f.get("wind_bearing"))
    return {
        "datetime":  label,
        "timestamp": ts,
        "date":      label,
        "skycon":    {"date": label, "value": skycon},
        "wind":      {"datetime": label, "value": {
            "wind_dir":   wind_dir,
            "wind_level": _wind_level(wind_speed),
            "speed":      wind_speed,
        }},
        "temperature": {"datetime": label, "timestamp": ts, "value": temperature},
        "humidity":    {"datetime": label, "timestamp": ts, "value": humidity},
        "pm25":        {"datetime": label, "timestamp": ts, "value": 0},
    }


async def _fetch_forecast(hass: HomeAssistant, weather_id: str,
                          kind: str) -> list[dict[str, Any]]:
    """Call HA's `weather.get_forecasts` service to pull the daily or
    hourly forecast array. Modern HA (2024.4+) moved forecasts off the
    weather entity's attributes into a service call; older HA versions
    keep them on `attributes.forecast`."""
    try:
        resp = await hass.services.async_call(
            "weather",
            "get_forecasts",
            {"entity_id": weather_id, "type": kind},
            blocking=True,
            return_response=True,
        )
        return list(resp.get(weather_id, {}).get("forecast", []) or [])
    except (HomeAssistantError, AttributeError, KeyError):
        # Fallback: legacy `forecast` attribute (no day/hour split).
        st = hass.states.get(weather_id)
        return list((st.attributes.get("forecast") if st else []) or [])


# --------------------------------------------------------------------------- #
# Publish                                                                     #
# --------------------------------------------------------------------------- #

def _mqtt_publish_payload(payload: Any) -> str:
    """JSON-encode keeping non-ASCII intact (city names, etc.). Compact
    output keeps the retained MQTT message small and matches the
    on-the-wire shape the stock app's parser sees from the real cloud."""
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


async def publish_weather(hass: HomeAssistant, mac: str,
                          cfg: dict[str, Any]) -> None:
    """Build all five endpoint payloads from current HA state and publish
    them to `qpext/<mac>/weather/<endpoint>/set`, retained. Safe to call
    on any state change — if a source entity is missing or invalid the
    affected fields fall back to zeros and the stock UI degrades quietly
    instead of clearing the widget."""
    weather_id = cfg.get(CONF_WEATHER_ENTITY)

    now      = _build_now()
    locate   = _build_locate(cfg, hass)
    weather  = _build_weather_now(cfg, hass)

    daily_fcs:  list[dict[str, Any]] = []
    hourly_fcs: list[dict[str, Any]] = []
    if weather_id:
        raw_daily  = await _fetch_forecast(hass, weather_id, "daily")
        raw_hourly = await _fetch_forecast(hass, weather_id, "hourly")
        daily_fcs  = [_daily_entry(f, hass) for f in raw_daily]
        hourly_fcs = [_hourly_entry(f) for f in raw_hourly]

    publishes = {
        "now":             now,
        "locate":          locate,
        "weatherNow":      weather,
        "dailyForecasts":  daily_fcs,
        "hourlyForecasts": hourly_fcs,
    }
    for endpoint, body in publishes.items():
        topic = WEATHER_TOPIC_TEMPLATE.format(mac=mac, endpoint=endpoint)
        try:
            await hass.services.async_call(
                "mqtt", "publish",
                {"topic": topic,
                 "payload": _mqtt_publish_payload(body),
                 "retain": True},
                blocking=True,
            )
        except HomeAssistantError as err:
            _LOGGER.error("qpext_airmonitor: publish %s failed: %s", topic, err)
            return

    _LOGGER.info(
        "qpext_airmonitor: weather pushed (daily=%d, hourly=%d)",
        len(daily_fcs), len(hourly_fcs),
    )
