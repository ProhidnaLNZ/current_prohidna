# 01 — System overview

## What the reader does

1. **Reads** an ISO 14443A card (MIFARE Classic / Ultralight / NTAG, etc.) with a PN532 NFC module.
2. **Decides locally** whether the card is allowed, using a whitelist of card UIDs cached in flash and synced from the backend.
3. **Signals** the decision immediately: green ring + rising melody (allowed) or red ring + falling melody (denied).
4. **Reports** every read (allowed *and* denied) to the backend as an MQTT event.
5. **Keeps working offline:** the decision does not need the network, and events are stored in a flash "outbox" and sent later.

The reader does **not** drive a door lock, relay or turnstile; there is no actuator output in the firmware. It is an identification + attendance + feedback terminal. Whether a physical barrier is controlled elsewhere is an [open question](10-roadmap-and-open-questions.md#open-questions).

## Platform context

```mermaid
flowchart LR
    subgraph School["School site (one per school)"]
        direction TB
        Card[/"Student / staff card<br/>(ISO 14443A)"/]
        R1["Prohidna Reader #1<br/>ESP32-S3 firmware"]
        R2["Prohidna Reader #2"]
        Rn["Prohidna Reader #N"]
        LAN[("School LAN<br/>Ethernet / Wi-Fi AP")]
        Card -. tap .-> R1
        R1 --- LAN
        R2 --- LAN
        Rn --- LAN
    end

    subgraph Platform["Prohidna platform"]
        direction TB
        Broker[["Mosquitto broker<br/>username/password auth + ACL"]]
        Workers["MQTT workers (Python)<br/>ingest reads, serve whitelist"]
        API["Python API"]
        DB[("Platform database")]
        Broker <--> Workers
        Workers --> DB
        API --> DB
    end

    NTP(["pool.ntp.org"])
    Users(["School staff / admins / parents apps"])

    LAN -- "MQTT over TCP" --> Broker
    LAN -- "NTP (UDP 123)" --> NTP
    Users --> API
```

The reader only talks to two external systems: the **MQTT broker** and an **NTP server**. All business logic beyond "is this UID on the list" lives in the backend.

### Deployment view

```plantuml
@startuml
title Prohidna — deployment view (one school)
skinparam componentStyle rectangle

node "Entrance (x N per school)" as entrance {
  node "Prohidna Reader\n(ESP32-S3)" as reader {
    component "Firmware\n(this repo)" as fw
    database "LittleFS\n/outbox.txt\n/whitelist.txt" as fs
    fw --> fs
  }
  component "PN532 NFC" as pn532
  component "WS2812 ring (24 LEDs)" as ring
  component "Passive buzzer" as buzzer
  pn532 -- reader : SPI (bit-banged)
  ring -- reader : 1-wire GRB
  buzzer -- reader : PWM tone
}

node "School network" as net {
  component "Switch (Ethernet)" as sw
  component "Wi-Fi AP (fallback)" as ap
}

cloud "Prohidna platform" {
  node "Mosquitto" as mq
  node "Python MQTT workers" as wk
  node "Python API" as api
  database "DB" as db
}

cloud "Internet" {
  node "pool.ntp.org" as ntp
}

reader -- sw : W5500 Ethernet (primary)
reader .. ap : Wi-Fi STA (fallback)
sw --> mq : MQTT :MQTT_PORT
ap --> mq
sw --> ntp : UDP 123
mq <--> wk
wk --> db
api --> db
@enduml
```

## Responsibilities split

| Concern | Reader firmware | Backend (Python API + workers) |
|---|---|---|
| Card identification | Reads UID (+ optional legacy 64-bit code from block 4) | Maps UID / legacy code to a person |
| Access decision | Local, from cached whitelist | Owns and publishes the whitelist |
| Feedback to person | LED ring + buzzer | — |
| Event history / attendance | Emits one event per read, buffers offline | Stores, deduplicates by event id, assigns date |
| Time | NTP → converts uptime to `HH-MM-SS` at send time | Must add the date (payload has none) |
| Device identity | **Hard-coded** (`reader_real`) — see risks | Should map device → school / entrance |

## Tech stack

| Layer | Choice |
|---|---|
| MCU | ESP32-S3 (PlatformIO board profile `esp32s3box`) |
| Framework | Arduino-ESP32 on FreeRTOS, built with PlatformIO (`platform = espressif32`, unpinned) |
| NFC | Adafruit PN532 library |
| LEDs | Adafruit NeoPixel (24 × WS2812, GRB) |
| Networking | Arduino `Ethernet` (W5500 over SPI) + ESP32 `WiFi` |
| Messaging | `PubSubClient` (MQTT 3.1.1, QoS 0 publish) |
| JSON | `ArduinoJson` (v7 API: `JsonDocument`) |
| Storage | LittleFS on internal flash |
| Language | C++ (header-only classes), comments partly in Ukrainian |

## Glossary

| Term | Meaning |
|---|---|
| **UID** | Card's unique identifier. Rendered by the firmware as decimal bytes joined with `-`, e.g. `4-123-55-201`. This exact string is what the whitelist must contain. |
| **Legacy code** | 64-bit number read from MIFARE Classic block 4 (default key `FF…FF`), only for 4-byte-UID cards. `0` when absent. |
| **Whitelist** | Set of allowed UID strings. Delivered by the backend over MQTT, cached in `/whitelist.txt`. |
| **Read event / record** | One line `uid:code:millis:event_id:allowed` created per accepted tap. |
| **Outbox** | `/outbox.txt` on flash; read events that could not be published yet. |
| **Event id** | 16-hex-digit random id (`generateUUID()`), the idempotency key for the backend. |
| **Stable network** | `NetworkManager::isStable()` — Ethernet or Wi-Fi is connected. |
| **WDT** | ESP32 task watchdog, 30 s, resets the chip if a watched task stops checking in. |

## Project status (for planning)

| Area | State |
|---|---|
| Core flow (read → decide → feedback → report) | Implemented |
| Offline operation (cached whitelist, outbox) | Implemented, with reliability gaps (see 09 #1, #3, #4) |
| Ethernet/Wi-Fi failover, NTP | Implemented |
| Multi-reader deployment (per-device identity) | **Not implemented** — blocker for "many readers per school" |
| Security (TLS, per-device credentials) | **Not implemented** |
| Second hardware variant (Olimex ESP32-POE) | Code present, not buildable without changes |
| Automated tests / CI | None |
| OTA firmware update, remote config, device health telemetry | None |
