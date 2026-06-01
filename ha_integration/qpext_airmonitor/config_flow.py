"""Config + options flow for Qpext Airmonitor.

The config flow only asks for the device MAC. All real configuration lives in
the options flow, which is a small multi-step UI for picking widgets via HA's
entity / icon selectors. Each change is persisted to `entry.options`, which
triggers the update listener in __init__.py and re-publishes to MQTT.
"""
from __future__ import annotations

import json
from typing import Any

import voluptuous as vol

from homeassistant import config_entries
from homeassistant.core import callback
from homeassistant.helpers import selector

from .const import (
    CONF_EVENTS,
    CONF_MAC,
    CONF_WIDGETS,
    DEFAULT_ICONS,
    DOMAIN,
    DOMAIN_FOR_WIDGET_TYPE,
    ENTITY_WIDGET_TYPES,
    TAB_NAMES,
    WIDGET_TYPES,
)


def _normalize_mac(raw: str) -> str:
    return raw.replace(":", "").replace("-", "").replace(" ", "").upper()


def _widget_label(idx: int, w: dict[str, Any]) -> str:
    """Human-readable label for the edit/remove selector."""
    t = w.get("type", "?")
    body = w.get("label") or w.get("entity") or w.get("service") or ""
    return f"#{idx + 1} · {t} · {body}".strip()


# --------------------------------------------------------------------------- #
# Config flow                                                                 #
# --------------------------------------------------------------------------- #


class QpextConfigFlow(config_entries.ConfigFlow, domain=DOMAIN):
    """Initial 'add integration' flow — just the device MAC."""

    VERSION = 1

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
                    title=f"Airmonitor {mac}",
                    data={CONF_MAC: mac},
                    options={CONF_WIDGETS: [], CONF_EVENTS: []},
                )
        return self.async_show_form(
            step_id="user",
            data_schema=vol.Schema({vol.Required(CONF_MAC): str}),
            errors=errors,
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
    """Multi-step UI for managing widgets and tab-switch event triggers.

    The flow keeps a working copy of the widget/event lists in instance
    attributes. Every add/edit/remove step calls `_persist()` which writes
    back to the config entry and triggers the update listener (-> MQTT
    publish). The user can leave the flow at any time without losing changes.
    """

    def __init__(self, entry: config_entries.ConfigEntry) -> None:
        self.entry = entry
        opts = entry.options or {}
        self._widgets: list[dict[str, Any]] = list(opts.get(CONF_WIDGETS, []))
        self._events: list[dict[str, Any]] = list(opts.get(CONF_EVENTS, []))
        # State carried across the multi-step "add"/"edit" pages.
        self._add_type: str | None = None
        self._edit_idx: int | None = None

    # ----- top menu ------------------------------------------------------- #

    async def async_step_init(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        # Show different menu options depending on whether anything to edit/remove.
        menu = ["add_widget"]
        if self._widgets:
            menu += ["edit_widget", "remove_widget"]
        menu += ["events_menu", "finish"]
        return self.async_show_menu(step_id="init", menu_options=menu)

    async def async_step_finish(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        """Close the flow. State is already persisted by intermediate steps."""
        return self.async_create_entry(
            title="",
            data={CONF_WIDGETS: self._widgets, CONF_EVENTS: self._events},
        )

    # ----- add widget ----------------------------------------------------- #

    async def async_step_add_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        """Pick the type of widget to add."""
        if user_input is not None:
            self._add_type = user_input["type"]
            if self._add_type == "button":
                return await self.async_step_add_button()
            return await self.async_step_add_entity_widget()

        schema = vol.Schema(
            {
                vol.Required("type"): selector.SelectSelector(
                    selector.SelectSelectorConfig(
                        options=WIDGET_TYPES,
                        mode=selector.SelectSelectorMode.DROPDOWN,
                        translation_key="widget_type",
                    )
                ),
            }
        )
        return self.async_show_form(step_id="add_widget", data_schema=schema)

    async def async_step_add_entity_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        """Add a widget for an HA entity (sensor / switch / light / etc.)."""
        wtype = self._add_type or "sensor"
        if user_input is not None:
            self._widgets.append(
                {
                    "type": wtype,
                    "entity": user_input["entity"],
                    "label": user_input.get("label") or "",
                    "icon": user_input.get("icon") or DEFAULT_ICONS.get(wtype, ""),
                }
            )
            self._add_type = None
            await self._persist()
            return await self.async_step_init()

        domain_filter = DOMAIN_FOR_WIDGET_TYPE.get(wtype)
        ent_cfg = (
            selector.EntitySelectorConfig(domain=domain_filter)
            if domain_filter
            else selector.EntitySelectorConfig()
        )
        schema = vol.Schema(
            {
                vol.Required("entity"): selector.EntitySelector(ent_cfg),
                vol.Optional("label", default=""): selector.TextSelector(),
                vol.Optional(
                    "icon", default=DEFAULT_ICONS.get(wtype, "")
                ): selector.IconSelector(),
            }
        )
        return self.async_show_form(
            step_id="add_entity_widget",
            data_schema=schema,
            description_placeholders={"widget_type": wtype},
        )

    async def async_step_add_button(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        """Add a button widget: free-form service call, no required entity."""
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
                self._widgets.append(widget)
                self._add_type = None
                await self._persist()
                return await self.async_step_init()

        schema = vol.Schema(
            {
                vol.Optional("label", default=""): selector.TextSelector(),
                vol.Required("service"): selector.TextSelector(),
                vol.Optional("data", default=""): selector.TextSelector(
                    selector.TextSelectorConfig(multiline=True)
                ),
                vol.Optional(
                    "icon", default=DEFAULT_ICONS["button"]
                ): selector.IconSelector(),
            }
        )
        return self.async_show_form(
            step_id="add_button", data_schema=schema, errors=errors
        )

    # ----- edit widget ---------------------------------------------------- #

    async def async_step_edit_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        """Pick which widget to edit."""
        if not self._widgets:
            return await self.async_step_init()
        if user_input is not None:
            self._edit_idx = int(user_input["index"])
            w = self._widgets[self._edit_idx]
            if w.get("type") == "button":
                return await self.async_step_edit_button()
            return await self.async_step_edit_entity_widget()
        return self.async_show_form(
            step_id="edit_widget",
            data_schema=self._index_picker_schema(self._widgets, _widget_label),
        )

    async def async_step_edit_entity_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        idx = self._edit_idx
        if idx is None or idx >= len(self._widgets):
            return await self.async_step_init()
        w = dict(self._widgets[idx])
        wtype = w.get("type", "sensor")

        if user_input is not None:
            w["entity"] = user_input["entity"]
            w["label"] = user_input.get("label") or ""
            w["icon"] = user_input.get("icon") or DEFAULT_ICONS.get(wtype, "")
            self._widgets[idx] = w
            self._edit_idx = None
            await self._persist()
            return await self.async_step_init()

        domain_filter = DOMAIN_FOR_WIDGET_TYPE.get(wtype)
        ent_cfg = (
            selector.EntitySelectorConfig(domain=domain_filter)
            if domain_filter
            else selector.EntitySelectorConfig()
        )
        schema = vol.Schema(
            {
                vol.Required("entity", default=w.get("entity", "")): selector.EntitySelector(
                    ent_cfg
                ),
                vol.Optional("label", default=w.get("label", "")): selector.TextSelector(),
                vol.Optional(
                    "icon", default=w.get("icon") or DEFAULT_ICONS.get(wtype, "")
                ): selector.IconSelector(),
            }
        )
        return self.async_show_form(
            step_id="edit_entity_widget",
            data_schema=schema,
            description_placeholders={"widget_type": wtype},
        )

    async def async_step_edit_button(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        idx = self._edit_idx
        if idx is None or idx >= len(self._widgets):
            return await self.async_step_init()
        w = dict(self._widgets[idx])
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
                self._widgets[idx] = w
                self._edit_idx = None
                await self._persist()
                return await self.async_step_init()

        existing_data = (
            json.dumps(w["data"], ensure_ascii=False) if w.get("data") else ""
        )
        schema = vol.Schema(
            {
                vol.Optional("label", default=w.get("label", "")): selector.TextSelector(),
                vol.Required("service", default=w.get("service", "")): selector.TextSelector(),
                vol.Optional("data", default=existing_data): selector.TextSelector(
                    selector.TextSelectorConfig(multiline=True)
                ),
                vol.Optional(
                    "icon", default=w.get("icon") or DEFAULT_ICONS["button"]
                ): selector.IconSelector(),
            }
        )
        return self.async_show_form(
            step_id="edit_button", data_schema=schema, errors=errors
        )

    # ----- remove widget -------------------------------------------------- #

    async def async_step_remove_widget(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        if not self._widgets:
            return await self.async_step_init()
        if user_input is not None:
            i = int(user_input["index"])
            if 0 <= i < len(self._widgets):
                del self._widgets[i]
                await self._persist()
            return await self.async_step_init()
        return self.async_show_form(
            step_id="remove_widget",
            data_schema=self._index_picker_schema(self._widgets, _widget_label),
        )

    # ----- events submenu ------------------------------------------------- #

    async def async_step_events_menu(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        menu = ["add_event"]
        if self._events:
            menu += ["remove_event"]
        menu += ["init"]  # 'back to main' is implicit
        return self.async_show_menu(step_id="events_menu", menu_options=menu)

    async def async_step_add_event(
        self, user_input: dict[str, Any] | None = None
    ) -> config_entries.ConfigFlowResult:
        if user_input is not None:
            self._events.append(
                {
                    "entity": user_input["entity"],
                    "switch_to": user_input["switch_to"],
                }
            )
            await self._persist()
            return await self.async_step_events_menu()

        schema = vol.Schema(
            {
                vol.Required("entity"): selector.EntitySelector(
                    selector.EntitySelectorConfig(
                        domain=["automation", "binary_sensor", "input_boolean", "sensor"]
                    )
                ),
                vol.Required("switch_to"): selector.SelectSelector(
                    selector.SelectSelectorConfig(
                        options=TAB_NAMES,
                        mode=selector.SelectSelectorMode.DROPDOWN,
                    )
                ),
            }
        )
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

    # ----- helpers -------------------------------------------------------- #

    @staticmethod
    def _index_picker_schema(items: list[dict], labeller) -> vol.Schema:
        options = [
            {"value": str(i), "label": labeller(i, item)}
            for i, item in enumerate(items)
        ]
        return vol.Schema(
            {
                vol.Required("index"): selector.SelectSelector(
                    selector.SelectSelectorConfig(
                        options=options, mode=selector.SelectSelectorMode.LIST
                    )
                )
            }
        )

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
        """Save current widgets/events to the entry's options.

        Triggers the update listener (in __init__.py), which republishes
        the dashboard via MQTT. This means changes apply to the device
        within the duration of the next QML poll (~1.5 s) — no explicit
        "save" needed; the final menu option just closes the flow.
        """
        self.hass.config_entries.async_update_entry(
            self.entry,
            options={CONF_WIDGETS: self._widgets, CONF_EVENTS: self._events},
        )
