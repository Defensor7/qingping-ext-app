"""Config + options flow for Airmonitor App Extension.

Two paths into the integration:

  1. **Auto-discovery via MQTT** (preferred). The shim publishes a retained
     presence message to `qpext/<mac>/info`; HA routes that to
     `async_step_mqtt`.
  2. **Manual entry**. The user enters the device's Wi-Fi MAC themselves.

All real configuration lives in the options flow, which is now organized
around TABS. A tab is either a `widgets` tab (a set of cards) or a
`camera` tab (a single RTSP feed). The integration publishes one
MQTT-discovery button per tab so HA exposes a "Show <tab name>" entity.

Menu shape:

  init  ── Add tab
        ├─ Edit tab           ── pick tab ─→ tab_menu
        ├─ Remove tab         ── pick tab ─→ removed, back to init
        ├─ Triggers (events)  ── events_menu (unchanged)
        └─ Done

  tab_menu (for a widgets-type tab):
    ├─ Rename tab
    ├─ Add widget
    ├─ Edit widget
    ├─ Remove widget
    └─ Back to init

  tab_menu (for a camera-type tab):
    ├─ Rename tab
    ├─ Edit camera
    └─ Back to init
"""
from __future__ import annotations

import json
import logging
from typing import Any, Callable

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.core import callback
from homeassistant.helpers import selector
from homeassistant.helpers.service_info.mqtt import MqttServiceInfo

from .const import (
    CONF_EVENTS,
    CONF_MAC,
    CONF_TABS,
    DEFAULT_ICONS,
    DEFAULT_TAB_ICON,
    DOMAIN,
    DOMAIN_FOR_WIDGET_TYPE,
    STOCK_TAB_NAMES,
    TAB_TYPE_CAMERA,
    TAB_TYPE_WIDGETS,
    TAB_TYPES,
    WIDGET_TYPES,
)
from .tabs import migrate_options, slugify, tab_qml_name, unique_id

_LOGGER = logging.getLogger(__name__)


def _normalize_mac(raw: str) -> str:
    return raw.replace(":", "").replace("-", "").replace(" ", "").upper()


def _widget_label(idx: int, w: dict[str, Any]) -> str:
    t = w.get("type", "?")
    body = w.get("label") or w.get("entity") or w.get("service") or ""
    return f"#{idx + 1} · {t} · {body}".strip()


def _tab_label(idx: int, t: dict[str, Any]) -> str:
    ttype = t.get("type", "?")
    n = len((t.get("widgets") or [])) if ttype == TAB_TYPE_WIDGETS else 1
    suffix = f"{n} widget(s)" if ttype == TAB_TYPE_WIDGETS else "camera"
    return f"#{idx + 1} · {t.get('name', t.get('id', '?'))} · {suffix}"


# Per-widget appearance overrides. The QML side (`widgets/Frame.qml`) reads
# these via readonly properties `titleColor` / `titleSize` / `valueColor` /
# `valueSize` / `iconColor` / `iconSize` / `bgColor`.
APPEARANCE_FIELDS = ("title_size", "title_color", "value_size", "value_color",
                     "icon_size", "icon_color", "bg_color")


def _appearance_extras(defaults: dict[str, Any] | None = None) -> dict:
    """Schema fragment (mergeable into a vol.Schema dict) for appearance
    overrides. Sizes are integers (0 = "use default"), colors are free-form
    strings — QML's `color` type tolerates `#rgb`, `#rrggbb`, and named
    CSS colors so we don't validate beyond non-empty."""
    d = defaults or {}
    return {
        vol.Optional("title_size", default=int(d.get("title_size", 0) or 0)):
            selector.NumberSelector(selector.NumberSelectorConfig(
                min=0, max=200, step=1, mode=selector.NumberSelectorMode.BOX)),
        vol.Optional("title_color", default=str(d.get("title_color", "") or "")):
            selector.TextSelector(),
        vol.Optional("value_size", default=int(d.get("value_size", 0) or 0)):
            selector.NumberSelector(selector.NumberSelectorConfig(
                min=0, max=200, step=1, mode=selector.NumberSelectorMode.BOX)),
        vol.Optional("value_color", default=str(d.get("value_color", "") or "")):
            selector.TextSelector(),
        vol.Optional("icon_size", default=int(d.get("icon_size", 0) or 0)):
            selector.NumberSelector(selector.NumberSelectorConfig(
                min=0, max=200, step=1, mode=selector.NumberSelectorMode.BOX)),
        vol.Optional("icon_color", default=str(d.get("icon_color", "") or "")):
            selector.TextSelector(),
        vol.Optional("bg_color", default=str(d.get("bg_color", "") or "")):
            selector.TextSelector(),
    }


def _extract_appearance(user_input: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {}
    for key in APPEARANCE_FIELDS:
        v = user_input.get(key)
        if v in (None, "", 0):
            continue
        if key.endswith("_size"):
            try:
                out[key] = int(v)
            except (TypeError, ValueError):
                continue
        else:
            out[key] = str(v).strip()
    return out


def _camera_url_redacted(url: str) -> str:
    if "@" not in url:
        return url
    head, tail = url.rsplit("@", 1)
    scheme, _, creds = head.partition("//")
    if ":" not in creds:
        return url
    user, _, _password = creds.partition(":")
    return f"{scheme}//{user}:•••@{tail}"


# --------------------------------------------------------------------------- #
# Config flow                                                                 #
# --------------------------------------------------------------------------- #


class QpextConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Initial 'add integration' flow — manual entry OR MQTT auto-discovery."""

    VERSION = 1

    def __init__(self) -> None:
        self._discovered_mac: str | None = None
        self._discovered_info: dict[str, Any] = {}

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            mac = _normalize_mac(user_input[CONF_MAC])
            if len(mac) != 12 or any(c not in "0123456789ABCDEF" for c in mac):
                errors[CONF_MAC] = "invalid_mac"
            else:
                await self.async_set_unique_id(mac)
                self._abort_if_unique_id_configured()
                return self.async_create_entry(
                    title=f"Airmonitor App Extension {mac}",
                    data={CONF_MAC: mac},
                    options={CONF_TABS: [], CONF_EVENTS: []},
                )
        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema({vol.Required(CONF_MAC): str}),
            errors=errors,
        )

    async def async_step_mqtt(
        self, discovery_info: MqttServiceInfo
    ) -> config_entries.ConfigFlowResult:
        topic = discovery_info.topic
        parts = topic.split("/")
        if len(parts) != 3 or parts[0] != "qpext" or parts[2] != "info":
            return self.async_abort(reason="invalid_discovery_topic")
        mac = _normalize_mac(parts[1])
        if len(mac) != 12 or any(c not in "0123456789ABCDEF" for c in mac):
            return self.async_abort(reason="invalid_mac")

        info: dict[str, Any] = {}
        if discovery_info.payload:
            try:
                info = json.loads(discovery_info.payload) or {}
            except ValueError:
                _LOGGER.debug("qpext_airmonitor: discovery payload not JSON: %r",
                              discovery_info.payload[:80])

        await self.async_set_unique_id(mac)
        self._abort_if_unique_id_configured()

        self._discovered_mac = mac
        self._discovered_info = info
        self.context["title_placeholders"] = {
            "name": f"Airmonitor App Extension {mac}",
        }
        return await self.async_step_discovery_confirm()

    async def async_step_discovery_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        assert self._discovered_mac is not None
        mac = self._discovered_mac
        if user_input is not None:
            return self.async_create_entry(
                title=f"Airmonitor App Extension {mac}",
                data={CONF_MAC: mac},
                options={CONF_TABS: [], CONF_EVENTS: []},
            )
        info = self._discovered_info
        return self.async_show_form(
            step_id="discovery_confirm",
            description_placeholders={
                "mac": mac,
                "model": info.get("model", "Air Monitor 2"),
                "sw": info.get("sw", "qpext"),
            },
        )

    @staticmethod
    @callback
    def async_get_options_flow(
        config_entry: config_entries.ConfigEntry,
    ) -> "QpextOptionsFlow":
        return QpextOptionsFlow(config_entry)


# --------------------------------------------------------------------------- #
# Options flow                                                                #
# --------------------------------------------------------------------------- #


class QpextOptionsFlow(config_entries.OptionsFlow):
    """Multi-step UI for managing tabs (widgets / camera) and event triggers.

    The flow keeps a working copy of the tabs/events lists. Every add/edit/
    remove step calls `_persist()`, which writes back to the config entry
    and triggers the update listener (-> MQTT publish). The user can leave
    the flow at any time without losing changes.
    """

    def __init__(self, entry: config_entries.ConfigEntry) -> None:
        self.entry = entry
        # Normalize legacy {widgets, cameras} into tabs on first open.
        normalized = migrate_options(entry.options or {})
        self._tabs: list[dict[str, Any]] = normalized[CONF_TABS]
        self._events: list[dict[str, Any]] = normalized[CONF_EVENTS]
        self._tab_idx: int | None = None
        self._add_type: str | None = None
        self._edit_widget_idx: int | None = None

    # ----- top menu ------------------------------------------------------- #

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        menu = ["add_tab"]
        if self._tabs:
            menu += ["edit_tab", "remove_tab"]
        menu += ["events_menu", "finish"]
        return self.async_show_menu(step_id="init", menu_options=menu)

    async def async_step_finish(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        return self.async_create_entry(
            title="",
            data={CONF_TABS: self._tabs, CONF_EVENTS: self._events},
        )

    # ----- add tab -------------------------------------------------------- #

    async def async_step_add_tab(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        """Pick a name + type for the new tab."""
        errors: dict[str, str] = {}
        if user_input is not None:
            name = (user_input.get("name") or "").strip()
            if not name:
                errors["name"] = "name_required"
            else:
                base_id = slugify(name)
                tid = unique_id(base_id, [t["id"] for t in self._tabs])
                ttype = user_input.get("type") or TAB_TYPE_WIDGETS
                icon = user_input.get("icon") or (
                    "mdi:cctv" if ttype == TAB_TYPE_CAMERA else DEFAULT_TAB_ICON)
                tab: dict[str, Any] = {
                    "id": tid, "name": name, "type": ttype, "icon": icon,
                }
                if ttype == TAB_TYPE_WIDGETS:
                    tab["widgets"] = []
                else:
                    tab["camera"] = {}
                self._tabs.append(tab)
                self._tab_idx = len(self._tabs) - 1
                await self._persist()
                # Drop straight into the tab's menu so the user can fill it.
                return await self.async_step_tab_menu()

        schema = vol.Schema({
            vol.Required("name"): selector.TextSelector(),
            vol.Required("type", default=TAB_TYPE_WIDGETS): selector.SelectSelector(
                selector.SelectSelectorConfig(
                    options=TAB_TYPES,
                    mode=selector.SelectSelectorMode.DROPDOWN,
                    translation_key="tab_type",
                )
            ),
            vol.Optional("icon", default=DEFAULT_TAB_ICON): selector.IconSelector(),
        })
        return self.async_show_form(
            step_id="add_tab", data_schema=schema, errors=errors)

    # ----- edit tab (pick + menu) ---------------------------------------- #

    async def async_step_edit_tab(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        if not self._tabs:
            return await self.async_step_init()
        if user_input is not None and "index" in user_input:
            self._tab_idx = int(user_input["index"])
            return await self.async_step_tab_menu()
        return self.async_show_form(
            step_id="edit_tab",
            data_schema=self._index_picker_schema(self._tabs, _tab_label),
        )

    async def async_step_tab_menu(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        idx = self._tab_idx
        if idx is None or idx >= len(self._tabs):
            return await self.async_step_init()
        tab = self._tabs[idx]
        menu = ["rename_tab"]
        if tab["type"] == TAB_TYPE_WIDGETS:
            menu += ["add_widget"]
            if tab.get("widgets"):
                menu += ["edit_widget", "remove_widget"]
        else:
            menu += ["edit_camera"]
        menu += ["init"]   # back to top
        return self.async_show_menu(
            step_id="tab_menu", menu_options=menu,
            description_placeholders={
                "tab_name": tab.get("name", tab.get("id", "?")),
                "tab_type": tab.get("type", "?"),
            },
        )

    async def async_step_rename_tab(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        idx = self._tab_idx
        if idx is None or idx >= len(self._tabs):
            return await self.async_step_init()
        tab = self._tabs[idx]
        if user_input is not None:
            name = (user_input.get("name") or "").strip() or tab.get("name", "")
            icon = (user_input.get("icon") or "").strip() or tab.get("icon", DEFAULT_TAB_ICON)
            tab["name"] = name
            tab["icon"] = icon
            await self._persist()
            return await self.async_step_tab_menu()
        schema = vol.Schema({
            vol.Required("name", default=tab.get("name", "")): selector.TextSelector(),
            vol.Optional("icon", default=tab.get("icon") or DEFAULT_TAB_ICON):
                selector.IconSelector(),
        })
        return self.async_show_form(
            step_id="rename_tab", data_schema=schema,
            description_placeholders={
                "tab_name": tab.get("name", tab.get("id", "?")),
            },
        )

    async def async_step_remove_tab(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        if not self._tabs:
            return await self.async_step_init()
        if user_input is not None and "index" in user_input:
            i = int(user_input["index"])
            if 0 <= i < len(self._tabs):
                del self._tabs[i]
                self._tab_idx = None
                await self._persist()
            return await self.async_step_init()
        return self.async_show_form(
            step_id="remove_tab",
            data_schema=self._index_picker_schema(self._tabs, _tab_label),
        )

    # ----- camera content (single-camera tab) ---------------------------- #

    async def async_step_edit_camera(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        idx = self._tab_idx
        if idx is None or idx >= len(self._tabs):
            return await self.async_step_init()
        tab = self._tabs[idx]
        if tab["type"] != TAB_TYPE_CAMERA:
            return await self.async_step_tab_menu()
        cur = tab.get("camera") or {}
        errors: dict[str, str] = {}
        if user_input is not None:
            cam = self._camera_from_input(user_input, fallback_id=tab["id"])
            if not cam["url"].startswith("rtsp://"):
                errors["url"] = "rtsp_only"
            else:
                tab["camera"] = cam
                await self._persist()
                return await self.async_step_tab_menu()
        return self.async_show_form(
            step_id="edit_camera",
            data_schema=self._camera_schema(cur),
            description_placeholders={
                "tab_name": tab.get("name", tab.get("id", "?")),
                "redacted": _camera_url_redacted(cur.get("url", "")) or "(none)",
            },
            errors=errors,
        )

    @staticmethod
    def _camera_schema(defaults: dict[str, Any] | None = None) -> "vol.Schema":
        d = defaults or {}
        return vol.Schema({
            vol.Required("url",  default=d.get("url",  "")): selector.TextSelector(
                selector.TextSelectorConfig(type=selector.TextSelectorType.URL)
            ),
            vol.Optional("label", default=d.get("label", "")): selector.TextSelector(),
            vol.Optional("fps", default=d.get("fps", 5)): selector.NumberSelector(
                selector.NumberSelectorConfig(min=1, max=30, step=1,
                                              mode=selector.NumberSelectorMode.BOX)
            ),
            vol.Optional("width", default=d.get("width", 480)): selector.NumberSelector(
                selector.NumberSelectorConfig(min=160, max=1920, step=80,
                                              mode=selector.NumberSelectorMode.BOX)
            ),
        })

    @staticmethod
    def _camera_from_input(
        user_input: dict[str, Any], fallback_id: str
    ) -> dict[str, Any]:
        # `name` on the device side is used as the snapshot filename — keep it
        # ASCII/safe. Tab id is already slugified so it makes a fine default.
        name = slugify(fallback_id, "cam")
        out = {
            "name":  name,
            "url":   (user_input.get("url") or "").strip(),
            "fps":   int(user_input.get("fps", 5) or 5),
            "width": int(user_input.get("width", 480) or 480),
        }
        if user_input.get("label"):
            out["label"] = user_input["label"]
        return out

    # ----- widget content (multi-widgets tab) ---------------------------- #

    async def async_step_add_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        if self._tab_idx is None:
            return await self.async_step_init()
        if user_input is not None:
            self._add_type = user_input["type"]
            if self._add_type == "button":
                return await self.async_step_add_button()
            return await self.async_step_add_entity_widget()
        schema = vol.Schema({
            vol.Required("type"): selector.SelectSelector(
                selector.SelectSelectorConfig(
                    options=WIDGET_TYPES,
                    mode=selector.SelectSelectorMode.DROPDOWN,
                    translation_key="widget_type",
                )
            ),
        })
        return self.async_show_form(step_id="add_widget", data_schema=schema)

    def _current_widgets(self) -> list[dict[str, Any]]:
        idx = self._tab_idx
        if idx is None or idx >= len(self._tabs):
            return []
        tab = self._tabs[idx]
        if tab.get("type") != TAB_TYPE_WIDGETS:
            return []
        return tab.setdefault("widgets", [])

    async def async_step_add_entity_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        wtype = self._add_type or "sensor"
        widgets = self._current_widgets()
        if user_input is not None:
            widget = {
                "type": wtype,
                "entity": user_input["entity"],
                "label": user_input.get("label") or "",
                "icon": user_input.get("icon") or DEFAULT_ICONS.get(wtype, ""),
            }
            widget.update(_extract_appearance(user_input))
            widgets.append(widget)
            self._add_type = None
            await self._persist()
            return await self.async_step_tab_menu()

        domain_filter = DOMAIN_FOR_WIDGET_TYPE.get(wtype)
        ent_cfg = (
            selector.EntitySelectorConfig(domain=domain_filter)
            if domain_filter
            else selector.EntitySelectorConfig()
        )
        schema = vol.Schema({
            vol.Required("entity"): selector.EntitySelector(ent_cfg),
            vol.Optional("label", default=""): selector.TextSelector(),
            vol.Optional(
                "icon", default=DEFAULT_ICONS.get(wtype, "")
            ): selector.IconSelector(),
            **_appearance_extras(),
        })
        return self.async_show_form(
            step_id="add_entity_widget",
            data_schema=schema,
            description_placeholders={"widget_type": wtype},
        )

    async def async_step_add_button(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        widgets = self._current_widgets()
        errors: dict[str, str] = {}
        if user_input is not None:
            data_obj = self._parse_service_data(user_input.get("data") or "", errors)
            if not errors:
                widget: dict[str, Any] = {
                    "type": "button",
                    "label": user_input.get("label") or "",
                    "service": user_input["service"],
                    "icon": user_input.get("icon") or DEFAULT_ICONS["button"],
                }
                if data_obj:
                    widget["data"] = data_obj
                widget.update(_extract_appearance(user_input))
                widgets.append(widget)
                self._add_type = None
                await self._persist()
                return await self.async_step_tab_menu()

        schema = vol.Schema({
            vol.Optional("label", default=""): selector.TextSelector(),
            vol.Required("service"): selector.TextSelector(),
            vol.Optional("data", default=""): selector.TextSelector(
                selector.TextSelectorConfig(multiline=True)
            ),
            vol.Optional(
                "icon", default=DEFAULT_ICONS["button"]
            ): selector.IconSelector(),
            **_appearance_extras(),
        })
        return self.async_show_form(
            step_id="add_button", data_schema=schema, errors=errors
        )

    async def async_step_edit_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        widgets = self._current_widgets()
        if not widgets:
            return await self.async_step_tab_menu()
        if user_input is not None:
            self._edit_widget_idx = int(user_input["index"])
            w = widgets[self._edit_widget_idx]
            if w.get("type") == "button":
                return await self.async_step_edit_button()
            return await self.async_step_edit_entity_widget()
        return self.async_show_form(
            step_id="edit_widget",
            data_schema=self._index_picker_schema(widgets, _widget_label),
        )

    async def async_step_edit_entity_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        widgets = self._current_widgets()
        i = self._edit_widget_idx
        if i is None or i >= len(widgets):
            return await self.async_step_tab_menu()
        w = dict(widgets[i])
        wtype = w.get("type", "sensor")
        if user_input is not None:
            w["entity"] = user_input["entity"]
            w["label"] = user_input.get("label") or ""
            w["icon"] = user_input.get("icon") or DEFAULT_ICONS.get(wtype, "")
            for k in APPEARANCE_FIELDS:
                w.pop(k, None)
            w.update(_extract_appearance(user_input))
            widgets[i] = w
            self._edit_widget_idx = None
            await self._persist()
            return await self.async_step_tab_menu()

        domain_filter = DOMAIN_FOR_WIDGET_TYPE.get(wtype)
        ent_cfg = (
            selector.EntitySelectorConfig(domain=domain_filter)
            if domain_filter
            else selector.EntitySelectorConfig()
        )
        schema = vol.Schema({
            vol.Required("entity", default=w.get("entity", "")): selector.EntitySelector(
                ent_cfg
            ),
            vol.Optional("label", default=w.get("label", "")): selector.TextSelector(),
            vol.Optional(
                "icon", default=w.get("icon") or DEFAULT_ICONS.get(wtype, "")
            ): selector.IconSelector(),
            **_appearance_extras(w),
        })
        return self.async_show_form(
            step_id="edit_entity_widget",
            data_schema=schema,
            description_placeholders={"widget_type": wtype},
        )

    async def async_step_edit_button(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        widgets = self._current_widgets()
        i = self._edit_widget_idx
        if i is None or i >= len(widgets):
            return await self.async_step_tab_menu()
        w = dict(widgets[i])
        errors: dict[str, str] = {}
        if user_input is not None:
            data_obj = self._parse_service_data(user_input.get("data") or "", errors)
            if not errors:
                w["label"] = user_input.get("label") or ""
                w["service"] = user_input["service"]
                w["icon"] = user_input.get("icon") or DEFAULT_ICONS["button"]
                if data_obj:
                    w["data"] = data_obj
                elif "data" in w:
                    del w["data"]
                for k in APPEARANCE_FIELDS:
                    w.pop(k, None)
                w.update(_extract_appearance(user_input))
                widgets[i] = w
                self._edit_widget_idx = None
                await self._persist()
                return await self.async_step_tab_menu()

        existing_data = (
            json.dumps(w["data"], ensure_ascii=False) if w.get("data") else ""
        )
        schema = vol.Schema({
            vol.Optional("label", default=w.get("label", "")): selector.TextSelector(),
            vol.Required("service", default=w.get("service", "")): selector.TextSelector(),
            vol.Optional("data", default=existing_data): selector.TextSelector(
                selector.TextSelectorConfig(multiline=True)
            ),
            vol.Optional(
                "icon", default=w.get("icon") or DEFAULT_ICONS["button"]
            ): selector.IconSelector(),
            **_appearance_extras(w),
        })
        return self.async_show_form(
            step_id="edit_button", data_schema=schema, errors=errors
        )

    async def async_step_remove_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        widgets = self._current_widgets()
        if not widgets:
            return await self.async_step_tab_menu()
        if user_input is not None:
            i = int(user_input["index"])
            if 0 <= i < len(widgets):
                del widgets[i]
                await self._persist()
            return await self.async_step_tab_menu()
        return self.async_show_form(
            step_id="remove_widget",
            data_schema=self._index_picker_schema(widgets, _widget_label),
        )

    # ----- events submenu ------------------------------------------------ #

    async def async_step_events_menu(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        menu = ["add_event"]
        if self._events:
            menu += ["remove_event"]
        menu += ["init"]
        return self.async_show_menu(step_id="events_menu", menu_options=menu)

    async def async_step_add_event(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        if user_input is not None:
            self._events.append({
                "entity": user_input["entity"],
                "switch_to": user_input["switch_to"],
            })
            await self._persist()
            return await self.async_step_events_menu()

        # Build the "switch_to" dropdown from stock tab names + user tabs.
        # User-tab QML names are `qpext_<tab.id>`; that's what MainPage.qml's
        # PathView model uses, so `switch_to` must match it exactly.
        choices = list(STOCK_TAB_NAMES)
        for t in self._tabs:
            choices.append(tab_qml_name(t["id"]))
        schema = vol.Schema({
            vol.Required("entity"): selector.EntitySelector(
                selector.EntitySelectorConfig(
                    domain=["automation", "binary_sensor", "input_boolean", "sensor"]
                )
            ),
            vol.Required("switch_to"): selector.SelectSelector(
                selector.SelectSelectorConfig(
                    options=choices,
                    mode=selector.SelectSelectorMode.DROPDOWN,
                )
            ),
        })
        return self.async_show_form(step_id="add_event", data_schema=schema)

    async def async_step_remove_event(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        if not self._events:
            return await self.async_step_events_menu()
        if user_input is not None:
            i = int(user_input["index"])
            if 0 <= i < len(self._events):
                del self._events[i]
                await self._persist()
            return await self.async_step_events_menu()

        def _event_label(idx: int, e: dict[str, Any]) -> str:
            return f"#{idx + 1} · {e.get('entity', '?')} → {e.get('switch_to', '?')}"

        return self.async_show_form(
            step_id="remove_event",
            data_schema=self._index_picker_schema(self._events, _event_label),
        )

    # ----- helpers ------------------------------------------------------- #

    @staticmethod
    def _index_picker_schema(
        items: list[dict], labeller: Callable[[int, dict[str, Any]], str]
    ) -> vol.Schema:
        options = [
            {"value": str(i), "label": labeller(i, item)}
            for i, item in enumerate(items)
        ]
        return vol.Schema({
            vol.Required("index"): selector.SelectSelector(
                selector.SelectSelectorConfig(
                    options=options, mode=selector.SelectSelectorMode.LIST
                )
            )
        })

    @staticmethod
    def _parse_service_data(raw: str, errors: dict[str, str]) -> dict[str, Any] | None:
        raw = raw.strip()
        if not raw:
            return None
        try:
            obj = json.loads(raw)
        except ValueError:
            errors["data"] = "data_invalid_json"
            return None
        if not isinstance(obj, dict):
            errors["data"] = "data_not_object"
            return None
        return obj

    async def _persist(self) -> None:
        """Save current tabs/events to the entry's options.

        Triggers the update listener (in __init__.py), which republishes the
        dashboard + cameras retained MQTT messages and syncs the per-tab
        discovery buttons. Changes apply to the device within the next QML
        poll (~1.5 s).
        """
        self.hass.config_entries.async_update_entry(
            self.entry,
            options={CONF_TABS: self._tabs, CONF_EVENTS: self._events},
        )
