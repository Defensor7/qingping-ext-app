# qpext_airmonitor — Home Assistant integration

Управление составом дашборда Qingping Snow2 (Air Monitor 2) прямо из HA
GUI: добавляешь/редактируешь/удаляешь виджеты через обычные HA-формы,
интеграция публикует JSON в retained-топик
`qpext/<mac>/dashboard/set`, шим на устройстве атомарно перезаписывает
`/data/qpext/widgets.json` (сохраняя локальный `ha.token`), QML
hot-reload'ит панель за ~1.5 с.

## Установка

1. Скопировать папку `qpext_airmonitor/` в
   `<ha-config>/custom_components/qpext_airmonitor/`.

   Структура должна быть:
   ```
   <ha-config>/
     custom_components/
       qpext_airmonitor/
         __init__.py
         config_flow.py
         const.py
         manifest.json
         services.yaml
         strings.json
         translations/
           en.json
   ```

2. Перезапустить Home Assistant.

3. Settings → Devices & Services → **Add integration** → искать
   *Qpext Airmonitor*. Ввести MAC устройства (12 hex-символов с
   разделителями или без — например `58:2D:34:70:A8:73` или
   `582D3470A873`). MAC берётся из `[device] wifi_mac` в
   `/data/etc/setting.ini` на устройстве.

4. После добавления у девайса появится опция **Configure** — это и есть
   главный UI: меню с кнопками *Add widget* / *Edit widget* /
   *Remove widget* / *Triggers* / *Done*.

## Виджеты

Поддерживаются те же типы, что в QML-стороне
([`qpext/qml/Plugins/widgets/`](../qpext/qml/Plugins/widgets/)):

| тип             | что делает                                  | поля                                     |
|-----------------|---------------------------------------------|------------------------------------------|
| `sensor`        | read-only значение                          | entity, label, icon                      |
| `switch`        | toggle on/off                               | entity, label, icon                      |
| `light`         | toggle + слайдер яркости                    | entity, label, icon                      |
| `climate`       | текущая темп + setpoint с +/-               | entity, label, icon                      |
| `media_player`  | play/pause/skip                             | entity, label, icon                      |
| `cover`         | open/close/stop                             | entity, label, icon                      |
| `script`        | запуск script.*                             | entity, label, icon                      |
| `scene`         | активация сцены                             | entity, label, icon                      |
| `button`        | произвольный service-call по тапу            | label, service, data (JSON), icon        |

Entity-селектор HA автоматически фильтруется по домену, соответствующему
типу виджета.

## Триггеры (events)

Подмножество HA-событий, которые на устройстве вызывают переключение
вкладки PathView. Используется для «звонок в дверь → показать камеру»
— автоматизация в HA, при срабатывании которой устройство автоматически
выводит на экран нужный экран:

- *Source HA entity* — обычно `automation.*`, может быть
  `binary_sensor.*`, `input_boolean.*` и т.п.
- *Switch device to tab* — одно из:
  `airDatasView`, `summaryView`, `settingView`, `appView`, `qpextView`,
  `qpextCamerasView`.

Срабатывает по изменению `attributes.last_triggered` (для automation)
либо `last_updated` / `state`.

## Сервисы

`qpext_airmonitor.republish` — повторно опубликовать текущий состав
дашборда. Полезно после wipe устройства, когда retained-сообщение в
брокере есть, но `widgets.json` локально пустой.

## Что НЕ передаётся через MQTT

`ha.base_url` и `ha.token` — они живут локально в
`/data/qpext/widgets.json` на устройстве и сохраняются между правками
из HA (шим сливает только остальные top-level ключи payload'а). Это
сознательное решение: чтобы случайно отозванный/протёкший токен
оставался в одном месте.

## Совместимость

Тестировалось на Home Assistant 2024.x+. Использует:
- `selector.EntitySelector` с фильтром по домену
- `selector.IconSelector` (доступен с HA 2023.x)
- `selector.TextSelector(multiline=True)`
- `OptionsFlow` с многошаговыми меню (`async_show_menu`)

Зависимость: `mqtt` (объявлена в `manifest.json`).
