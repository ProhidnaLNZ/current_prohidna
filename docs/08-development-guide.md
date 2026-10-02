# 08 — Development guide

## Prerequisites

- [PlatformIO](https://platformio.org/) — either the VS Code extension or the CLI (`pip install platformio`).
- USB cable to the board. On Linux, add yourself to the `dialout` group (or install PlatformIO's udev rules).
- Optional: a local Mosquitto broker and `mosquitto-clients` for end-to-end testing.

PlatformIO downloads the toolchain, Arduino-ESP32 core and all libraries from `platformio.ini` on first build.

## 1. Create `include/secrets.h`

This file is gitignored and **required** — the build fails without it.

```cpp
#pragma once

#define WIFI_SSID   "school-iot"
#define WIFI_PASS   "change-me"

#define MQTT_SERVER "192.168.1.10"   // broker host or IP (const char*)
#define MQTT_PORT   1883             // int
#define MQTT_USER   "reader"
#define MQTT_PASS   "change-me"
```

Never commit it, and never paste a real one into chats, issues or public diagram renderers.

## 2. Build, flash, monitor

```sh
pio run                              # compile (env: esp32dev)
pio run -t upload                    # flash firmware
pio device monitor                   # serial console, 115200 baud
pio run -t upload -t monitor         # flash and attach
pio device monitor -f esp32_exception_decoder   # decode crash backtraces
pio run -t erase                     # erase all flash, incl. LittleFS (outbox + whitelist cache)
```

There is no test suite yet (`test/` contains only the PlatformIO template README).

## 3. What a healthy boot looks like

```text
--- Starting Setup ---
Initializing Task Watchdog with timeout of 30 seconds
Whitelist: Loaded 312 cards from cache.        (or "No cache file.")
NFC: Attempting to connect/reconnect to PN532...
NFC: Connection successful!
--- Starting Tasks on Specific Cores ---
--- Setup Complete. All tasks are running. ---
NetMan: Initializing... Attempting Ethernet.
NetMan: Ethernet Connected. IP: 192.168.1.57
NTP: Time synchronized.
MQTT: Attempting to connect...
MQTT: Connection SUCCESSFUL!
MQTT: Subscribed to whitelist response topic OK
```

### Log prefixes

| Prefix | Module | Typical problems it reveals |
|---|---|---|
| `NFC:` | `Nfc` | PN532 wiring/SPI mode, health-check failures, block 4 auth failures, full queue |
| `Whitelist:` | `Whitelist` | Cache load, chunk progress, JSON parse failures |
| `MQTT:` / `MQTT Task:` | `MqttClient` | Connect failures with `rc=` ([PubSubClient state codes](https://pubsubclient.knolleary.net/api#state): `-2` network, `4` bad credentials, `5` not authorised) |
| `Found N messages in outbox` / `Sent:` | outbox replay | Backlog size after an outage |
| `NetMan:` | `NetworkManager` | Ethernet/DHCP, Wi-Fi failover |
| `NTP:` | `NetworkManager` | Time sync (blocked UDP 123?) |
| `[WiFi-event]` | `main.cpp` | Raw Wi-Fi events, disconnect reason codes |

## 4. Local end-to-end test with Mosquitto

```sh
# broker with password auth (Mosquitto 2.x refuses anonymous by default)
mkdir -p mq && cd mq
mosquitto_passwd -c -b passwd reader change-me
printf 'listener 1883\npassword_file /mosquitto/config/passwd\n' > mosquitto.conf
docker run --rm -p 1883:1883 -v "$PWD":/mosquitto/config eclipse-mosquitto:2
```

Point `MQTT_SERVER` at your machine, flash, then in another terminal:

```sh
# watch everything the reader sends
mosquitto_sub -h localhost -u reader -P change-me -t 'device/#' -v

# answer a whitelist request (single message)
mosquitto_pub -h localhost -u reader -P change-me \
  -t device/reader_real/whitelist/response -m '["4-123-55-201","4-9-200-17"]'

# or chunked
mosquitto_pub -h localhost -u reader -P change-me -t device/reader_real/whitelist/response -m 'CHUNK:0:2:["4-123-55-201",'
mosquitto_pub -h localhost -u reader -P change-me -t device/reader_real/whitelist/response -m 'CHUNK:1:2:"4-9-200-17"]'
```

Tap a card, read its UID from `NFC: Processing new read. UID: ...`, add it to the list, publish again, and tap again: the ring should turn green.

Offline test: stop the broker, tap a few cards (reads go to the outbox), start the broker, and within ~5 s you should see `Found N messages in outbox`.

## 5. Code conventions in this repo

- **Header-only modules** in `include/`, one class per header, `#pragma once`. Only `main.cpp` (and `db.cpp`) are compiled. Headers that define non-inline functions or static members (`mqtt.h`, `timer.h`, `network_manager_olimex.h`) must be included from **one** `.cpp` only.
- **Globals live in `main.cpp`.** Modules get their collaborators through `setup(...)` references; they don't create each other.
- **Tick functions, not blocking loops.** Each module exposes a `read()` / `process()` / `iteration()` / `update()` / `handleConnection()` that does a bit of work and returns; the task wraps it in `for(;;){ tick(); vTaskDelay(...); }`.
- **Timing with `millis()` differences** (`millis() - last > INTERVAL`), never `delay()` inside tasks.
- **Logging** with `Serial.printf("Prefix: ...")` using the module prefix.
- **Constants** as `static constexpr` in the class; pins as `#define` in the pinout header.
- Naming is mixed (`snake_case_` members in `Nfc`/`MqttClient`, `_leadingUnderscore` in `DataBase`/`Whitelist`/`NetworkManager`). Follow the style of the file you edit.

## 6. Adding a new module (checklist)

1. Create `include/<module>.h` with a class exposing `setup(...)` and a tick method.
2. Instantiate it as a global in `main.cpp`; call `setup()` in `setup()` **after** its dependencies (LittleFS is mounted by `db.setup()`).
3. Run it from an existing task if it is cheap, or create a pinned task (core 0 = network side, core 1 = user-facing side). Choose a stack size and measure with `uxTaskGetStackHighWaterMark()`.
4. If it shares data with another task, protect it (queue, mutex) — see the [concurrency model](03-firmware-architecture.md#concurrency-model).
5. If its loop can block > a few seconds and the task is watched, call `esp_task_wdt_reset()`.
6. Update [03](03-firmware-architecture.md) (module + task tables).

## 7. Changing the read event format (checklist)

The record format is a contract shared by four places — change them together:

1. Producer: string building in [`include/nfc.h`](../include/nfc.h#L170).
2. Time substitution: [`replaceMillisWithTime()`](../include/timer.h) assumes `millis` is the **3rd** colon-separated field.
3. Queue item size `sizeof(char[64])` in [`src/main.cpp:101`](../src/main.cpp#L101) and `received_data[64]` in `MqttClient::process()`.
4. Backend parser + [07 — MQTT contract](07-mqtt-contract.md). Old records may still be in readers' outboxes — keep the backend tolerant of the previous format.

## Switching hardware variant

The Olimex variant is not wired in. To build it:

1. Add an environment to `platformio.ini` (the Olimex board is a classic ESP32, not an S3):

   ```ini
   [env:olimex-esp32-poe]
   platform = espressif32
   board = esp32-poe
   framework = arduino
   lib_deps = ${env:esp32dev.lib_deps}
   build_flags = -D MQTT_QOS=1
   monitor_speed = 115200
   ```

2. Replace `waveshare_pinout.h` → `olimex_pinout.h` in `src/main.cpp` and `include/nfc.h`, and `network_manager.h` → `network_manager_olimex.h` in `src/main.cpp` and `include/mqtt.h`.
3. Call `networkManager.setup();` in `setup()` before the network task starts (registers the ETH event handlers).

Better long-term: select the variant with a build flag (`-D BOARD_OLIMEX`) and one `board.h` that includes the right pinout and network manager, so both envs build from the same sources.
