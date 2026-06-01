# Local debug stack for `qpext_airmonitor`

Поднимает локальный Home Assistant + Mosquitto в Docker для отладки
интеграции `qpext_airmonitor` без участия твоего продового HA.

## Что внутри

| файл                  | что делает                                               |
|-----------------------|----------------------------------------------------------|
| `docker-compose.yml`  | HA на `:8123` + Mosquitto на `:1883`. Интеграция из `ha_integration/qpext_airmonitor` смонтирована read-only как custom_component. |
| `mosquitto.conf`      | `allow_anonymous true` — для отладки, в проде так не делать. |
| `bootstrap_ha.py`     | Stdlib-only: ждёт пока HA поднимется, проходит онбординг (admin/admin), добавляет MQTT-интеграцию с broker=mqtt, минтит long-lived access token через WebSocket-API. |
| `run.sh`              | `docker compose up -d` + bootstrap + печать summary.     |
| `stop.sh`             | `docker compose down`. Флаг `--wipe` сносит `ha-config/`. |
| `switch-device.sh`    | Бэкапит `/data/qpext/{mqtt,widgets}.json` на устройстве (`.prod`), перенастраивает их на адрес Mac'а и long-lived token, перезапускает приложение. |
| `restore-device.sh`   | Восстанавливает `.prod`-бэкапы.                           |

## Quick start

```sh
dev/run.sh up                       # поднять стек, прогнать онбординг
open http://localhost:8123          # admin / admin
dev/switch-device.sh                # развернуть Snow2 на этот HA
# поиграть с авто-дискавери и options flow в HA UI
dev/restore-device.sh               # вернуть устройство на прод
dev/stop.sh                         # остановить (ha-config сохраняется)
dev/stop.sh --wipe                  # полный reset
```

## Что нужно

- Docker (Docker Desktop на macOS) + `docker compose`
- Python 3.x на хосте (для `bootstrap_ha.py`)
- adb с доступным устройством (`adb devices` показывает `MSNS2D400E404501`)
- Mac должен быть в той же сети с устройством — `switch-device.sh`
  автоопределяет LAN-IP через `ipconfig getifaddr en0/en1`.

## Как это работает целиком

```
        ┌──────────────────┐
        │  Snow2 device    │
        │  shim qpext.so   │
        │                  │
        │  reads:          │
        │  /data/qpext/    │
        │    mqtt.json     │ ◀──── dev/switch-device.sh переписывает на
        │    widgets.json  │       host=<Mac>:1883 (anon) и
        └────────┬─────────┘       ha.{base_url,token}=<Mac>:8123/<llt>
                 │ MQTT
                 ▼
        ┌──────────────────┐
        │  Mosquitto        │ container `mqtt`, host:1883
        └────────┬─────────┘
                 │
                 ▼
        ┌──────────────────┐
        │  Home Assistant   │ container `homeassistant`, host:8123
        │  custom_components/
        │    qpext_airmonitor│ ◀── bind-mount из репы (RO)
        │                  │
        │  manifest:       │
        │  "mqtt": [       │
        │   "qpext/+/info" │  → MQTT integration субскрайбится,
        │  ]               │     при retained-presence от шима
        │                  │     роутит в async_step_mqtt
        │                  │  → flow в Discovered, юзер кликает
        │                  │     Configure → готовая конфиг-entry
        └──────────────────┘
```

## Сценарии отладки

- **Изменения в config_flow.py**: после правки → `docker restart qpext-dev-ha`
  → HA подтянет свежий код (mount read-only, перезагрузка не нужна,
  но HA кеширует модули — рестарт надёжнее).
- **Изменения в шиме**: `qpext/build.sh deploy` как обычно. Шим
  переподключится к debug-брокеру автоматически.
- **Видеть, что шлёт устройство**:
  ```sh
  docker run --rm --network qpext-dev_default eclipse-mosquitto:2 \
      mosquitto_sub -h mqtt -t 'qpext/+/#' -v
  ```
- **Watch HA логи**: `dev/run.sh logs` или `docker logs -f qpext-dev-ha`.
- **Список текущих discovery-flow'ов (без UI)**:
  ```sh
  python3 -c "
  import json, sys; sys.path.insert(0, 'dev'); import bootstrap_ha as B
  tok = open('dev/ha-config/.qpext-token').read().strip()
  ws = B._ws_connect('localhost', 8123, '/api/websocket')
  json.loads(B._ws_recv(ws))
  B._ws_send(ws, json.dumps({'type':'auth','access_token':tok}))
  json.loads(B._ws_recv(ws))
  B._ws_send(ws, json.dumps({'id':1,'type':'config_entries/flow/progress'}))
  print(json.dumps(json.loads(B._ws_recv(ws))['result'], indent=2))"
  ```

## Состояние и persistance

Всё persistent живёт в `dev/ha-config/` (gitignored). Между запусками
онбординг проходить не надо — `bootstrap_ha.py` идемпотентный: если
admin уже создан, он логинится паролем и переминтит long-lived token.
Чтобы получить чистое состояние HA — `dev/stop.sh --wipe`.

## Безопасность

- Mosquitto в анонимном режиме, слушает на 0.0.0.0:1883 → любой в локалке
  может подписаться/публиковать. Только для отладки.
- Long-lived token имеет 10-летний lifespan, лежит в
  `ha-config/.qpext-token` (chmod 600). При `stop.sh --wipe` инвалидируется
  вместе с базой HA.
- Это **debug-стек**, не для прода.
