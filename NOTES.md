# qpext — патчинг QingSnow2App без перекомпиляции

Заметки по тому, что было сделано. Цель: добавить в монитор воздуха
**Qingping Air Monitor 2 (Snow2)** свои QML-вкладки (виджеты Home Assistant и
живое видео с RTSP-камер) и MQTT-телеметрию, не пересобирая прошивку и не
трогая оригинальный бинарь на диске.

Устройство: ARM64 Linux, Qt 5.14.2, прошивка предоставляет приложение
`/qingping/bin/QingSnow2App` (динамически слинкованное, не stripped).
adb-доступ есть.

> **Prerequisites.** Доступ к устройству (root shell, adb, отключение
> облачной телеметрии) поднимается по гайду
> [ea/cgs2_decloud](https://github.com/ea/cgs2_decloud) — обязательное
> чтение, прежде чем тут что-то деплоить. Сопроводительный блогпост:
> [QingPing CGS2 De-cloud](https://blog.29b.net/dispatches/cgs2_decloud/).
> Эта работа продолжает оттуда: ставит LD_PRELOAD-шим поверх их рутового
> доступа и добавляет HA-виджеты, RTSP-камеры и MQTT-телеметрию.

---

## 1. Архитектура целиком

```
                                ┌──────────────────────────────┐
                                │  /qingping/bin/QingSnow2App  │  shell wrapper
                                │     export LD_PRELOAD=...    │
                                │     exec  ...App.real         │
                                └──────────────┬───────────────┘
                                               │
                                               ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │                  QingSnow2App.real  (Qt 5.14.2)                  │
   │                                                                  │
   │   constructor of qpext.so ──► spawn 3 фоновых pthread:           │
   │      • HA WebSocket   (state.json, tab_event)                    │
   │      • Camera launcher + snapshot + HTTP heartbeat + events      │
   │      • MQTT client     (HA discovery, telemetry, cmd)            │
   │                                                                  │
   │   QQmlApplicationEngine::load(QUrl) — перехвачен ──► load(QString│
   │   с подменённым URL  "file:///data/qpext/main.qml"               │
   └────────────────────────────┬─────────────────────────────────────┘
                                │
                                ▼
   ┌──────────────────────────────────────────────────────────────────┐
   │   /data/qpext/main.qml     зеркало qrc:/main.qml                 │
   │       └─► mainPage Loader ► file:///data/qpext/MainPage.qml      │
   │                                                                  │
   │   /data/qpext/MainPage.qml зеркало qrc:/Main/MainPage.qml +      │
   │       └─► две доп. вкладки PathView:                             │
   │             - file:///data/qpext/Plugins/Hello.qml    (HA)       │
   │             - file:///data/qpext/Plugins/Cameras.qml (RTSP)      │
   │       └─► poll  /tmp/qpext/grab.req    → screenshot.png          │
   │       └─► poll  /tmp/qpext/tab_event   → программное переключение│
   └──────────────────────────────────────────────────────────────────┘
```

Все три IPC-канала между `qpext.so` и QML — это **локальные файлы**,
которые QML опрашивает через `XMLHttpRequest` с `file://`. Это даёт
hot-reload и упрощает отладку.

---

## 2. Реверс-инжиниринг

Что узнавали из самого бинаря, прежде чем писать шим.

### 2.1 Точка входа QML

`find_qml_init.py` — ищет в `.text` все вызовы интересных Qt-символов
через PLT, восстанавливая мангленые имена. Подтвердило, что приложение
вызывает `QQmlApplicationEngine::load(const QUrl&)`. Это и есть точка для
LD_PRELOAD-хука.

`dump_qml_init.py` — печатает дизассемблер участка, где происходит
инициализация QML-движка, с расшифровкой `bl` через `.rela.plt → .dynsym`
и резолвом `adrp+add → строковый литерал` в `.rodata`.

### 2.2 Контекст для QML (что доступно глобально)

`dump_ctx.py` — реконструирует все
`engine.rootContext().setContextProperty(name, obj)` и `qmlRegister*<T>(...)`.
Это нужно было, чтобы в наших QML-файлах правильно использовать `global`,
`screenManager`, `datetimeManager`, `updateController`, `Styles`,
`QStackView`, `QText`, `Qing.Controls`, `Qing.Frames`, `Qing.Snow` и т.д.

### 2.3 Извлечение QML-ресурсов

`extract_qrc.py` — находит ВСЕ вызовы
`_Z21qRegisterResourceDataiPKhS0_S0_` в `.text`, восстанавливает 4 аргумента
(`version, tree*, names*, data*`) через трассировку `adrp+add` и парсит
qrc-деревья Qt. Результат лежит в `qrc_out/` — это полная распакованная
исходная QML-плёнка приложения. Из неё мы скопировали `qrc:/main.qml` и
`qrc:/Main/MainPage.qml` как стартовые шаблоны для своих модификаций.

### 2.4 Расположение `QString` в Qt 5.14 (важно для шима)

QArrayData header на aarch64:

```
off 0..3   : ref (int)
off 4..7   : size (int)        ← UTF-16 char count
off 8..11  : alloc bitfield
off 12..15 : padding (align)
off 16..23 : offset (qptrdiff) ← смещение UTF-16-данных от `d`
```

Используется в `qstring_to_utf8()` в `qpext.cpp` — для логирования сообщений
из `qInstallMessageHandler`. Без выгрузки исходников Qt не было способа узнать
это, кроме как прочитать `QArrayDataPointer<ushort>` глазами.

---

## 3. LD_PRELOAD-шим `qpext.so`

Три исходника, собирается на устройстве по `deploy.sh` (см. ниже).

### 3.1 `qpext.cpp` — hook движка QML

Главное:

- **Подмена `QQmlApplicationEngine::load(QUrl)`.** Мы экспортируем функцию с
  ровно таким манглом, и она получает приоритет над символом в `libQt5Qml`.
  Внутри читаем настоящий `load(QString)` и `QString::QString(QChar*, int)`
  через `dlsym(RTLD_NEXT, ...)` и вызываем `load(QString)` с
  `file:///data/qpext/main.qml`.

  **Почему не `load(QUrl)` напрямую:** `QUrl` нетривиален, занимает регистр
  `x8` (sret), `~QUrl` не экспортирован в Qt 5.14. Идти через `QString`
  избавляет от sret и не требует деструктора (`QString` течёт ровно один раз
  за жизнь процесса — это нормально).

- **`QPEXT_ENTRY` env** — позволяет перенаправить QML-вход на любой другой
  путь без передеплоя `.so`.

- **`qInstallMessageHandler`** — все `console.log`, `qWarning`, `qCritical`
  из QML летят в `/data/qpext/qpext.log` через UTF-8-декодер `QString`.
  Без этого отлаживать QML вслепую было бы невозможно: stderr уходит в
  `/dev/console` → UART, который наружу не выведен.

- **`in_target_process()`** через `/proc/self/comm` — `LD_PRELOAD`
  наследуется всеми форками (`wpa_cli`, `mosquitto_sub`, `gst-launch`), но
  фоновые треды и логи нужны только в `QingSnow2App.real`. Имя в `comm`
  обрезается до 15 символов: `"QingSnow2App.re"`.

### 3.2 `qpext_ha.cpp` — Home Assistant WebSocket-клиент

Без сторонних библиотек: ручной RFC 6455 (handshake → frame),
JSON через [jsmn](https://github.com/zserge/jsmn) (`jsmn.h` рядом).

Поток:

```
1. parse_config(): /data/qpext/widgets.json -> {host, port, path, token}
2. TCP connect → WebSocket Upgrade (handshake пишет остаток буфера в ibuf)
3. wait auth_required → send {"type":"auth","access_token":...}
4. wait auth_ok
5. subscribe_events(state_changed) + get_states  (snapshot)
6. цикл: каждый event/result обновляет g_states[entity_id] -> raw JSON,
   атомарно дампится в /data/qpext/state.json (rename) → QML видит через
   200-мс file:// poll.
```

Реконнект с экспоненциальным бекоффом до 30s.

**Триггеры тоже здесь** (`check_event_trigger` + `events_config_thread_fn`):
секция `events` из `widgets.json` мапит `entity_id → switch_to`. Когда у
entity меняется `attributes.last_triggered` / `last_updated` / `state`,
шим пишет `/tmp/qpext/tab_event` (JSON с `switch_to` + `ts`), MainPage.qml
поллит этот файл каждые 250 мс и переключает PathView на нужную вкладку.

Это и есть автоматизация «звонок в дверь → показать камеру».

### 3.3 `qpext_ha.cpp` — пайплайны камер

Запуск `gst-launch-1.0`:

```
rtspsrc location=URL latency=200 protocols=tcp
 ! rtph264depay ! h264parse
 ! mppvideodec               # hw H.264 (Rockchip MPP)
 ! videorate ! video/x-raw,framerate=FPS/1
 ! videoscale add-borders=false ! video/x-raw,width=W
 ! videoconvert ! mppjpegenc # hw JPEG
 ! multifilesink location=/tmp/qpext/cam/<name>-%d.jpg
                max-files=4 next-file=buffer
```

`max-files=4` + ротация индекса даёт QML безопасный «не на этом тике
переписывают» файл. Отдельный snapshot-тред каждые 100 мс находит
самый свежий `<name>-NNN.jpg` (`mtime`, size > 1 KB чтобы пропустить
полузаписанный) и атомарно копирует его в стабильное имя
`<name>.jpg` (`copy + rename`). QML грузит `Image` по этому имени с
`?<timestamp>`-cache-buster.

**Pause/resume** через HTTP-биение. На `127.0.0.1:8765` висит тривиальный
TCP-сервер; QML из активной вкладки `Cameras` дёргает `GET /heartbeat`
раз в 1.5 с. Если шим не слышал биение 3 с — `SIGSTOP` всем
`gst-launch`, экономит ~30% CPU. Биение возвращается — `SIGCONT`.

`PR_SET_PDEATHSIG` гарантирует, что `gst-launch`-дети умрут вместе с
приложением, а не останутся зомби на устройстве.

### 3.4 `qpext_mqtt.cpp` — MQTT-клиент

Своя реализация MQTT 3.1.1 (`CONNECT`, `PUBLISH`, `SUBSCRIBE`, `PINGREQ`).
Креды берутся из `/data/etc/setting.ini` (там уже есть рабочий broker для
оригинального приложения Qingping → переиспользуем).

Что публикуется:

- **HA Discovery** (`homeassistant/sensor/...` + `.../button/...` retained):
  - `soc_temp`, `gpu_temp`, `battery_temp` — `/sys/class/thermal/thermal_zone[0-2]/temp`
  - `cpu` — `/proc/stat` дельта
  - `ram_free` — `MemAvailable` из `/proc/meminfo`
  - `uptime` — `/proc/uptime`
  - `cam_status` — список `name:running|paused`, читается через
    `qpext_get_cam_status_into()` (мьютекс с тредом камер)
  - кнопки: `Snow2 reboot`, `Snow2 show camera`

- Телеметрия публикуется раз в 10 с.

- Подписка на `qpext/<mac>/cmd`. Поддерживаются:
  - `{"action":"reboot"}` → `sync()` + fork→`reboot`
  - `{"action":"switch_tab","name":"qpextCamerasView"}` → пишет тот же
    `/tmp/qpext/tab_event` что и event-триггеры HA.
- Публикация retained `qpext/<mac>/info` на каждый (ре)коннект. Payload —
  JSON-объект с метаданными устройства (`mac`, `model`, `device_id`,
  `dashboard_topic`, `cmd_topic`). Используется HA-интеграцией для
  auto-discovery: `manifest.json` декларирует `"mqtt": ["qpext/+/info"]`,
  HA читает retained-сообщение и автоматически предлагает добавить
  устройство в *Settings → Devices & Services* — без ручного ввода MAC.

- Подписка на `qpext/<mac>/dashboard/set` (retained). Payload — JSON-объект
  с любым набором top-level ключей (`widgets`, `events`, ...); шим
  атомарно перезаписывает `/data/qpext/widgets.json`, **сохраняя
  локальный `ha` блок** (токен и base_url никогда не ходят через MQTT).
  QML hot-reload подхватывает изменения в течение 1.5 с. Пустой/невалидный
  payload игнорируется с предупреждением в логе.

  Использование со стороны HA — через service call `mqtt.publish`:
  ```yaml
  service: mqtt.publish
  data:
    topic: qpext/<mac>/dashboard/set
    retain: true
    payload: |
      {"widgets":[{"type":"sensor","entity":"sensor.x","label":"X","icon":"..."}],
       "events":[{"entity":"automation.y","switch_to":"qpextCamerasView"}]}
  ```
  Или вручную через `mosquitto_pub -r -f config.json -t qpext/<mac>/dashboard/set`.
  HA-text entity для UI-редактирования не делали: дефолтный лимит
  payload'а у `mqtt.text` — 255 символов, а конфиг обычно крупнее.

Идентификатор устройства в HA — `qpext_<mac без двоеточий>`, MAC берётся
из `[device] wifi_mac`.

---

## 4. QML-слой

### 4.1 Зеркалирование `main.qml` и `MainPage.qml`

Идея: взять оригинальный файл из `qrc_out/`, переписать **dir-импорты** в
абсолютные `qrc:/...` (потому что наш файл лежит вне qrc и относительные
импорты не работают), оставить остальное как есть, и дописать одну строчку:

```qml
// main.qml
Component {
    id: mainPage
    Loader { source: "file:///data/qpext/MainPage.qml" }   // ← оригинал был qrc:/Main/MainPage.qml
}
```

```qml
// MainPage.qml  — внутри ListModel у PathView
ListElement { name: "qpextView";         source: "file:///data/qpext/Plugins/Hello.qml" }
ListElement { name: "qpextCamerasView";  source: "file:///data/qpext/Plugins/Cameras.qml" }
```

### 4.2 Особенности, которые пришлось обойти

- **Скриншоты для отладки.** Внутри `MainPage.qml` есть таймер, который раз
  в 500 мс читает `/tmp/qpext/grab.req`. При смене значения вызывает
  `control.grabToImage(...).saveToFile("/tmp/qpext/grab.png")`. Cкрипт
  `qpext/grab.sh` делает `echo $(date +%s) > /tmp/qpext/grab.req`, ждёт
  секунду и `adb pull` забирает PNG. Без этого UI отлаживать практически
  невозможно — экран маленький, fps низкий.

- **PathView `currentIndex` смещён на единицу от визуального индекса.**
  Path — это `PathLine startX=-w/2 relativeX=w*count`. С дефолтным
  `preferredHighlightBegin=0` элемент с индексом `currentIndex` снапается
  в `startX=-w/2`, что полностью слева за пределами экрана. Визуально
  по центру оказывается СЛЕДУЮЩИЙ элемент:

  ```
  visual_idx = (currentIndex + 1) mod n
  ```

  Это уже учтено в штатном коде `PageIndicator` —
  `currentIndex: (view.currentIndex + 1) % model.count`. Чтобы
  программно переключиться на визуальную вкладку `j`, надо ставить:

  ```js
  view.currentIndex = (j - 1 + n) mod n
  ```

  PathView сам анимирует оффсет до канонической `(n - targetCI) mod n`,
  и визуально item `j` оказывается в центре. Прямая установка
  `view.currentIndex = j` приводила к тому, что после дрейфа оффсета на
  канонический визуально показывалось `(j+1) mod n`, а внутренний
  `currentIndex` показывал `j` — что выглядело как «уже на вкладке»
  при повторном нажатии.

- **`view.currentIndex = 0` в `Component.onCompleted`** оставлен, но
  технически он визуально показывает item 1 (summaryView). Поскольку в
  модели summaryView вставляется только если `datetimeManager.isInitialized`,
  а до того момента визуально item 1 — это settingView, исходная логика
  «открываемся на первом экране» работает по случайности (или по
  отдельному эмпирическому фиту, который мы не разбирали).

- **`tab_event` poll** — 250 мс. Идемпотентность через `ts` в JSON: каждый
  обработанный таймстемп запоминается в `qpextLastTs`.

### 4.3 Hot-reload (`Hello.qml` / `Cameras.qml`)

Каждый «плагин» — это тонкий wrapper, который грузит реальный код через
`Loader`:

```qml
Loader { source: "file:///data/qpext/Plugins/HelloImpl.qml?v=" + shell.version }
```

`?v=N` обходит компонентный кэш Qt — иначе Qt не перечитывал бы файл.
Раз в 1.5 с wrapper делает `XMLHttpRequest` на тот же путь, считает дешёвую
сигнатуру (`length + первые 64 + последние 64 байта`) и при изменении
инкрементит `version`, что заставляет Loader перезагрузиться.

Это даёт workflow:

```
$ vim qpext/qml/Plugins/HelloImpl.qml
$ qpext/deploy.sh push-qml      # adb push --sync, без рестарта приложения
# через ~1.5 с экран сам перерисуется новой версией
```

Для проверки, что reload прошёл, в каждом `*Impl.qml` есть
`readonly property int revision` — её просто инкрементят и видят в
заголовке вкладки и в `qpext.log`.

### 4.4 Виджеты HA в `HelloImpl.qml`

- `widgets.json` хот-перечитывается (1.5 с poll, сигнатурное сравнение).
- `state.json` (от шима) поллится каждые 250 мс. Парсится в `haState`,
  инкрементится `haTick` — это триггер ребиндинга у всех виджетов.
- В `Repeater` динамический `Loader` подгружает `widgets/<Type>.qml` —
  `snake_case` маппится в `CamelCase`: `media_player` → `MediaPlayer.qml`.
- Виджет получает `widget` (конфиг) и `hass` (entity raw state) через
  `Binding`. Сервисы Home Assistant вызываются обычным REST-эндпоинтом
  (`POST /api/services/<domain>/<service>` с `Authorization: Bearer ...`)
  напрямую из QML — отдельный канал, не через WebSocket-сессию шима.

Иконки — Material Design Icons (`mdi.ttf`) через `MdiIcon.qml`, имена
маппятся в кодпоинты прямо в QML-словаре (расширяется руками).

Сетка виджетов завёрнута в `Flickable` с вертикальным скроллом. У
каждого виджета фиксированная `Layout.preferredHeight = impl.widgetHeight`
(180 по умолчанию), `GridLayout.implicitHeight` даёт `contentHeight`,
и при переполнении справа отрисовывается тонкий scroll-индикатор
(простой `Rectangle` без зависимости от QtQuick.Controls 2). Если
какой-то виджет требует больше места — переопределяй `implicitHeight`
у его `Frame`-корня и подними `impl.widgetHeight`: `GridLayout`
выравнивает ряды по самой высокой ячейке.

### 4.5 Камеры в `CamerasImpl.qml`

- `SwipeView` по `cameras.json`.
- Один `Image`, перезаписывается каждые `1000/fps` мс. **Хитрость:**
  Qt держит старый pixmap пока новый источник не догрузился, поэтому
  явно сбрасываем `img.source = ""` перед новым URL — иначе мерцает
  «застывший кадр».
- Heartbeat на `127.0.0.1:8765/heartbeat` пока `active=true`. `active`
  пробрасывается из `MainPage.qml`-делегата свойством `isVisualCurrent`
  через `Binding { target: loader.item; property: "isVisualCurrent"; ... }`,
  отгороженный по `objectName === "qpextCamerasShell"`, чтобы не
  лезть к qrc-вкладкам. `PathView.isCurrentItem` использовать **нельзя**
  из-за off-by-one (см. выше): он `true` для `ci=3` (qpextView), а не
  для камеры, и без правильного активного флага шим SIGSTOP'ит gst-launch
  через 3 с и поток замирает.
- В подвале показываем `fps · cpu% · °C` для дебага.

---

## 5. Деплой

`qpext/deploy.sh` — единый CLI:

| команда         | что делает |
|-----------------|------------|
| `install` (def.)| `adb push` всего дерева + ставит wrapper `/qingping/bin/QingSnow2App` (sh-обёртка, что задаёт `LD_PRELOAD`). Первый раз — переименовывает оригинал в `.real`. Идемпотентно. |
| `push-qml`      | Синкает только `qml/`. Hot-reload подхватит в ~1.5 с, app **не перезапускается**. |
| `undo`          | Восстанавливает `.real` → исходный путь. |
| `logs`          | `tail` штатного app-лога + watchdog-лог + `ps`. |
| `status`        | Файлы, размер, head wrapper-скрипта, `ps`. |

Watchdog устройства сам поднимает приложение через ~5 с после `pkill`,
поэтому `install` именно так и работает — `pkill && подожди`.

`qpext/grab.sh [out.png]` — описано выше, дёргает screenshot через QML и
тянет PNG.

`qpext/qpext.ver` — `Qt_5 { global: _ZN21QQmlApplicationEngine4loadERK4QUrl; local: *; }` —
version-script для линкера: наружу видна только наша единственная функция,
всё остальное `local` — иначе линкер мог бы зацепить случайные символы из
`qpext_ha.cpp` / `qpext_mqtt.cpp`.

---

## 6. Файлы и где что лежит

```
qingping/
├── QingSnow2App                ELF aarch64, копия с устройства для RE
├── device_libs/                libQt5Core.so / libQt5Qml.so оттуда же
├── tools/                      Ghidra + Java 21 + pyghidra venv
│
├── dump_ctx.py                 RE: восстановить контекстные регистрации
├── dump_qml_init.py            RE: дизасм QML init
├── find_qml_init.py            RE: найти call-sites Qt-символов
├── extract_qrc.py              RE: распаковать встроенный qrc
├── qrc_out/                    результат extract_qrc — исходные QML/JS
│
└── qpext/                      ВСЁ, ЧТО ДЕПЛОИТСЯ НА УСТРОЙСТВО
    ├── deploy.sh
    ├── grab.sh
    ├── qpext.ver               version-script линкера
    ├── jsmn.h                  vendored JSON parser (single-header)
    │
    ├── qpext.cpp               LD_PRELOAD шим: hook load() + лог + бутстрап
    ├── qpext_ha.cpp            HA WebSocket + camera launcher/snapshot/HTTP + events
    ├── qpext_mqtt.cpp          MQTT 3.1.1 + HA discovery + telemetry + cmd
    ├── qpext.so                собранный артефакт (на устройстве: /data/qpext/)
    │
    └── qml/                    пушается целиком в /data/qpext/
        ├── main.qml            зеркало qrc:/main.qml + Loader на наш MainPage
        ├── MainPage.qml        зеркало qrc:/Main/MainPage.qml + наши вкладки
        ├── widgets.json        конфиг: ha.{base_url,token}, widgets[], events[]
        ├── cameras.json        конфиг: cameras[] = [{name,label,url,fps,width}]
        │
        ├── Header/             переэкспорт qrc-компонентов (Header нужен MainPage)
        ├── Notification/       то же самое для NotificationBar
        ├── fonts/mdi.ttf       Material Design Icons
        │
        └── Plugins/
            ├── Hello.qml           hot-reload wrapper
            ├── HelloImpl.qml      ┃ ВКЛАДКА HA: poll widgets.json+state.json,
            │                      ┃ Repeater→Loader по типу
            ├── Cameras.qml         hot-reload wrapper
            ├── CamerasImpl.qml    ┃ ВКЛАДКА КАМЕР: SwipeView, Image, heartbeat
            ├── MdiIcon.qml         иконочный шрифт
            └── widgets/
                ├── Frame.qml       базовая «карточка», ловит tap
                ├── Light.qml       on/off + слайдер яркости
                ├── Switch.qml      toggle
                ├── Sensor.qml      read-only значение + единица
                ├── Climate.qml     режим + setpoint
                ├── Cover.qml       open/close/stop
                ├── MediaPlayer.qml play/pause/skip
                ├── Script.qml      запуск script.*
                ├── Button.qml      произвольный service-call по тапу
                └── Scene.qml       (stub)
```

Расположение на устройстве:

```
/qingping/bin/QingSnow2App        — sh-wrapper (наш), 1 строка с LD_PRELOAD + exec
/qingping/bin/QingSnow2App.real   — оригинальный бинарь
/qingping/bin/QingSnow2App.bak    — заводской бэкап (был до нас)
/data/qpext/qpext.so              — наш шим
/data/qpext/main.qml              — точка входа QML
/data/qpext/MainPage.qml          — модифицированная стартовая страница
/data/qpext/Plugins/...           — наши вкладки
/data/qpext/widgets.json          — конфиг HA
/data/qpext/cameras.json          — конфиг камер
/data/qpext/qpext.log             — лог шима + QML console.log
/data/qpext/state.json            — снапшот HA (генерится шимом)
/tmp/qpext/tab_event              — doorbell-style переключение вкладок
/tmp/qpext/grab.req               — триггер скриншота (mtime-based)
/tmp/qpext/grab.png               — PNG скриншота
/tmp/qpext/cam/<name>-NNN.jpg     — ротация JPEG-кадров от gstreamer
/tmp/qpext/cam/<name>.jpg         — стабильное имя для QML (atomic rename)
/tmp/qpext/cam/<name>.log         — stdout/stderr gst-launch для конкретной камеры
```

---

## 7. IPC-каналы, кратко

| канал                       | направление       | формат | период       |
|-----------------------------|-------------------|--------|--------------|
| `/data/qpext/state.json`    | shim → QML        | JSON   | 250 ms poll  |
| `/data/qpext/widgets.json`  | пользователь → оба| JSON   | 1.5 s sig poll |
| `/data/qpext/cameras.json`  | пользователь → оба| JSON   | 1.5 s sig poll |
| `/tmp/qpext/tab_event`      | shim → QML        | JSON   | 250 ms poll  |
| `/tmp/qpext/grab.req`       | adb → QML         | mtime  | 500 ms poll  |
| `/tmp/qpext/grab.png`       | QML → adb         | PNG    | on demand    |
| `/tmp/qpext/cam/<n>.jpg`    | shim → QML        | JPEG   | ≤ fps Hz     |
| `127.0.0.1:8765/heartbeat`  | QML → shim        | HTTP   | 1.5 s        |
| MQTT `qpext/<mac>/cmd`      | HA → shim         | JSON   | event        |
| HA WebSocket                | HA ↔ shim         | JSON   | persistent   |
| HA REST `/api/services/...` | QML → HA          | JSON   | по нажатию   |

---

## 8. Что не сделано / TODO

- TLS для WebSocket: сейчас только plain `http://host:8123`. Для HA в LAN
  ок, наружу не годится.
- Один MQTT-юзер захардкожен (тот, что в `setting.ini`). При смене
  broker'а — править INI.
- Виджеты `Scene` и часть атрибутов `MediaPlayer` — placeholder.
- Нет TLS/SRTP для RTSP. `protocols=tcp` нужен только потому что многие IP-камеры
  не принимают UDP-транспорт.
- `MdiIcon` глифы — захардкожены в QML; можно либо вынести в `mdi.json`,
  либо сгенерить полный набор. Сейчас расширяется руками.
- При очень частой ротации `widgets.json`/`cameras.json` теоретически
  возможна гонка между сигнатурным сравнением и парсом. На практике файл
  пишут руками — не проблема.
