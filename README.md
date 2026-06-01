# qingping-ext-app

Расширение для **Qingping Air Monitor 2** (он же CGS2 / Snow2): добавляет в
штатное приложение `QingSnow2App` дашборд Home Assistant, превью с
RTSP-камер и MQTT-телеметрию **без пересборки прошивки** — через
`LD_PRELOAD`-шим, который перенаправляет загрузку QML-движка на дерево
на диске.

> **Prerequisite:** root и доступ adb поднимаются по гайду
> [ea/cgs2_decloud](https://github.com/ea/cgs2_decloud) (блогпост:
> [QingPing CGS2 De-cloud](https://blog.29b.net/dispatches/cgs2_decloud/)).
> Эта работа продолжает оттуда.

## Что внутри

```
qpext/                       LD_PRELOAD-шим (C++) + QML, всё что
                             деплоится на устройство в /data/qpext/
  build.sh                   сборка qpext.so через zig (cross aarch64)
  deploy.sh                  push QML / install wrapper / pkill
  qpext.cpp                  hook QQmlApplicationEngine::load(QUrl)
  qpext_ha.cpp               WebSocket-клиент HA + камера-пайплайны (gst)
  qpext_mqtt.cpp             MQTT 3.1.1 + HA discovery + dashboard topic
  qml/                       main.qml, MainPage.qml, Plugins/, Header/, ...
  qml/*.json.example         плейсхолдеры; реальные .json в .gitignore

ha_integration/qpext_airmonitor/
                             Home Assistant custom_component
                             (config flow + options UI для управления
                             составом дашборда из HA)

NOTES.md                     полные технические заметки (архитектура,
                             IPC, RE-скрипты, off-by-one PathView и пр.)
```

## Быстрый старт

1. Получить root по [cgs2_decloud](https://github.com/ea/cgs2_decloud).
   После этого `adb shell` доступен, MQTT-credentials лежат в
   `/data/etc/setting.ini`.

2. Скопировать примеры и заполнить:
   ```sh
   cp qpext/qml/cameras.json.example qpext/qml/cameras.json
   cp qpext/qml/widgets.json.example qpext/qml/widgets.json
   # отредактировать оба: вставить RTSP-URL с логином/паролем,
   # base_url HA и long-lived access token
   ```

3. Собрать и задеплоить шим + QML:
   ```sh
   qpext/build.sh deploy        # сборка qpext.so + push + pkill
   qpext/deploy.sh install      # вешает sh-обёртку с LD_PRELOAD
   qpext/deploy.sh logs         # тейл /data/qpext/qpext.log
   ```

4. (Опционально) поставить HA-интеграцию:
   ```sh
   cp -r ha_integration/qpext_airmonitor \
         /<ha-config-dir>/custom_components/
   # перезапустить HA, добавить интеграцию в Settings -> Devices & Services
   ```

## Что устройство получает в HA после установки

- Сенсоры: SoC / GPU / battery temperature, CPU%, RAM free, uptime,
  camera status (всё `qpext_<mac>_*`)
- Кнопки: reboot + по кнопке на каждую вкладку приложения
  (`show_air`, `show_summary`, `show_settings`, `show_app`, `show_ha`,
  `show_camera`)
- Девайс под именем `Airmonitor (qpext)` — обе кнопки и сенсоры группируются
  под ним
- Топик `qpext/<mac>/dashboard/set` (retained) — куда HA-интеграция шлёт
  состав дашборда

## Подробности

См. [NOTES.md](NOTES.md) — там разобраны:
- LD_PRELOAD-хук `QQmlApplicationEngine::load(QUrl)` и почему через `QString`
- IPC между шимом и QML (опросом файлов в `/tmp/qpext/`)
- Off-by-one в `PathView` и формула `targetCI = (visualIdx - 1 + n) mod n`
- Pause/resume gst-launch по heartbeat'у, исключая SIGSTOP при свернутом окне
- Hot-reload QML без рестарта (`?v=N` cache-buster + сигнатурный poll)
- Сборка через `zig c++ -target aarch64-linux-gnu.2.31` (libstdc++ статически)

## Licence

TBD.
