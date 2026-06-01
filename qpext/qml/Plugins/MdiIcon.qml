// Material Design Icons via /data/qpext/fonts/mdi.ttf.
// Pass either a known short name ("lightbulb") or a hex codepoint
// like "f0335".
import QtQuick 2.9

Text {
    id: icon
    property string name: ""
    property real size: 28

    // Generated from @mdi/svg meta.json — extend as needed.
    readonly property var glyphs: ({
        "alert":             "f0026",
        "battery":           "f0079",
        "battery-charging":  "f0084",
        "bell":              "f009a",
        "bell-off":          "f009b",
        "blur":              "f00b5",
        "ceiling-light":     "f0769",
        "desk-lamp":         "f095f",
        "door":              "f081a",
        "door-open":         "f081c",
        "fan":               "f0210",
        "fan-off":           "f081d",
        "fire":              "f0238",
        "fish":              "f023a",
        "flash":             "f0241",
        "garage":            "f06d9",
        "help":              "f02d6",
        "home":              "f02dc",
        "led-strip":         "f07d6",
        "lightbulb":         "f0335",
        "lightbulb-off":     "f0e4f",
        "lock":              "f033e",
        "lock-open":         "f033f",
        "molecule-co2":      "f07e4",
        "music":             "f075a",
        "pause":             "f03e4",
        "play":              "f040a",
        "play-circle":       "f040c",
        "power":             "f0425",
        "radiator":          "f0438",
        "robot":             "f06a9",
        "run":               "f070e",
        "script":            "f0bc1",
        "script-text":       "f0bc2",
        "skip-next":         "f04ad",
        "skip-previous":     "f04ae",
        "snowflake":         "f0717",
        "spotify":           "f04c7",
        "stop":              "f04db",
        "television":        "f0502",
        "thermometer":       "f050f",
        "thermostat":        "f0393",
        "toggle-switch":     "f0521",
        "toggle-switch-off": "f0522",
        "umbrella":          "f054a",
        "valve":             "f1066",
        "volume-high":       "f057e",
        "volume-mute":       "f075f",
        "wall-sconce":       "f091c",
        "water":             "f058c",
        "water-percent":     "f058e",
        "weather-cloudy":    "f0590",
        "weather-sunny":     "f0599",
        "wifi":              "f05a9",
        "window-closed":     "f05ae",
        "window-open":       "f05b1",
        "youtube":           "f05c3"
    })

    function resolve(n) {
        if (!n) return ""
        if (n.indexOf("mdi-") === 0) n = n.substring(4)
        if (n.indexOf("mdi:") === 0) n = n.substring(4)
        var cp = glyphs[n]
        if (!cp) cp = /^[0-9a-fA-F]+$/.test(n) ? n : glyphs["help"]
        return String.fromCodePoint(parseInt(cp, 16))
    }

    font.family: mdiFontLoader.name
    font.pixelSize: size
    color: "white"
    horizontalAlignment: Text.AlignHCenter
    verticalAlignment: Text.AlignVCenter
    text: resolve(name)
}
