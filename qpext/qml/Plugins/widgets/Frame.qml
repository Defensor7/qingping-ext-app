// Shared visual frame for all widgets. Children go into `body` via the
// default property; tap is handled by the inner MouseArea (override `onTapped`).
import QtQuick 2.9

Rectangle {
    id: base
    property var  widget               // config from widgets.json
    property var  hass                // current HA entity state (may be null)
    property bool on: false            // visual highlight, set by descendants
    property bool available: !!hass || (widget && widget.type === "button") ||
                              (widget && widget.type === "script") ||
                              (widget && widget.type === "scene")

    // Display name shown in the widget header. Priority:
    //   1. widget.label    — explicitly set by the user (HA options flow)
    //   2. hass.attributes.friendly_name — HA's customised name for the entity
    //   3. widget.entity   — raw entity_id, last-resort fallback
    // We expose this as a readonly property so every widget can just bind
    // to `displayName` instead of repeating the chain.
    readonly property string displayName: {
        if (widget && widget.label) return widget.label
        if (hass && hass.attributes && hass.attributes.friendly_name)
            return hass.attributes.friendly_name
        return (widget && widget.entity) || ""
    }

    // Per-card appearance overrides, configurable in the HA options flow
    // (config_flow.py — `_appearance_extras`). Each widget binds to these
    // via `w.titleColor || <default>` / `w.titleSize || <default>` etc.,
    // so unset values fall back to the widget's own hardcoded defaults.
    readonly property string titleColor: (widget && widget.title_color) || ""
    readonly property int    titleSize:  (widget && widget.title_size)  || 0
    readonly property string valueColor: (widget && widget.value_color) || ""
    readonly property int    valueSize:  (widget && widget.value_size)  || 0
    readonly property string iconColor:  (widget && widget.icon_color)  || ""
    readonly property int    iconSize:   (widget && widget.icon_size)   || 0
    readonly property string bgColor:    (widget && widget.bg_color)    || ""
    signal call(string domain, string service, var data)
    signal tapped()

    default property alias bodyChildren: body.children

    radius: 8
    // bgColor (from widget.bg_color) is a full override — it wins over the
    // available / on-state coloring. Leave empty to keep the default logic.
    color: bgColor || (!available ? "#1a1a1a" : (on ? "#1f5588" : "#181818"))
    border.width: 1
    border.color: !available ? "#2a2a2a" : (on ? "#3388cc" : "#2a2a2a")

    // tap MouseArea is declared BEFORE body so the body's child MouseAreas
    // (Light's brightness slider, Climate's ±, MediaPlayer's transport keys)
    // sit on top in QML's stacking order and intercept their clicks first.
    // Clicks on empty card area fall through to this one → base.tapped().
    MouseArea {
        id: tap
        anchors.fill: parent
        onClicked: base.tapped()
        onPressAndHold: base.longPressed && base.longPressed()
    }
    property var longPressed   // optional override

    Item {
        id: body
        anchors.fill: parent
        anchors.margins: 9
    }
}
