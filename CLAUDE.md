# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32-S3 firmware (PlatformIO, Arduino framework) for "Prohidna" — an NFC access-card reader. It reads cards via a PN532, checks them against a server-synced whitelist, signals approve/deny with a NeoPixel ring and buzzer, and reports every read over MQTT. Code comments are partly in Ukrainian.

Detailed developer docs live in `docs/` (index: `docs/README.md`). `docs/07-mqtt-contract.md` is the integration spec for the Python backend: update it whenever topics or payload formats change. `docs/09-known-issues-and-risks.md` lists known defects by stable ID.

## Commands

Single env `esp32dev` (board `esp32s3box`) in `platformio.ini`:

```sh
pio run                     # build
pio run -t upload           # flash
pio device monitor          # serial log, 115200 baud
pio run -t upload -t monitor
```

No tests exist (`test/` holds only the PlatformIO README).

**Build requires `include/secrets.h`** (gitignored, not in repo). It must define `WIFI_SSID`, `WIFI_PASS`, `MQTT_SERVER`, `MQTT_PORT`, `MQTT_USER`, `MQTT_PASS`.

## Architecture

Nearly all logic is header-only classes in `include/`; `src/main.cpp` wires global singletons and starts FreeRTOS tasks pinned to cores:

- **Network Task** (core 0) — `NetworkManager` state machine: try Ethernet (W5500 over SPI) → on timeout/link loss fall back to Wi-Fi → back to Ethernet, with a transition delay. Syncs NTP on connect (manual UDP NTP for Ethernet, `configTime` for Wi-Fi). `isStable()` gates all MQTT activity.
- **NFC Reader Task** (core 1) — `Nfc::read()`: polls PN532, reconnects on health-check failure, debounces same-UID reads (`CARD_READ_COOLDOWN_MS`), decides allow/deny via `Whitelist`, optionally reads a legacy 64-bit code from MIFARE Classic block 4, and pushes a record onto `readQueue`.
- **Housekeeping Task** (core 1) — non-blocking `Led::iteration()` / `Buzzer::update()` animation ticks.
- **MQTT Client Task** (core 0, watchdog-registered) — `MqttClient::process()`: drains `readQueue`, publishes live if connected, otherwise appends to the LittleFS outbox (`DataBase`, `/outbox.txt`); every 5 s replays the outbox and removes sent lines; requests whitelist sync when `Whitelist::needsSync()`.

Key data flow details:
- Read record format (queue/outbox): `uid:custom_code:millis:uuid:allowed`. Before publishing, `replaceMillisWithTime()` (`timer.h`) swaps the 3rd field for `HH-MM-SS` wall-clock time derived from the first NTP sync — so the outbox stores millis, not time, and reboots lose that mapping.
- Queue items are `char[64]` (`xQueueCreate(50, sizeof(char[64]))`); records longer than 63 chars are truncated.
- `networkClientMutex` (global, `extern` in headers) serializes access to the network client between the network and MQTT tasks.
- Whitelist sync: publish empty payload to `device/reader_real/whitelist/request`; server responds on `.../whitelist/response` with either a JSON array of UIDs or chunked `CHUNK:<index>:<total>:<json_part>` messages that are concatenated then parsed. Cached in LittleFS `/whitelist.txt`, resynced every 24 h. UIDs are decimal bytes joined by `-` (e.g. `4-123-55-201`). Reads are denied while the whitelist isn't READY.
- Task watchdog: 30 s timeout, panics on expiry. Long blocking calls in watched tasks (main loop, MQTT task) must call `esp_task_wdt_reset()`.

## Hardware variants

- `waveshare_pinout.h` + `network_manager.h` — the active build (SPI Ethernet via `Ethernet` lib).
- `olimex_pinout.h` + `network_manager_olimex.h` — alternative board with LAN8720 RMII via `ETH.h`. Not wired in; switching means changing the includes in `main.cpp`/`nfc.h`/`network_manager.h` usage. Both define the same `NetworkManager` class, so never include both.

## Gotchas

- `mqtt.h` (static member definition) and `timer.h` (non-inline functions) define symbols in headers: include them from only one translation unit (`main.cpp`) or you get duplicate-symbol link errors.
- Pinout headers lack include guards.
