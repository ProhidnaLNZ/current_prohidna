# 06 — Networking

`NetworkManager` ([`include/network_manager.h`](../include/network_manager.h)) runs in its own task on core 0 and ticks every 500 ms. It owns link selection (Ethernet first, Wi-Fi fallback), DHCP and NTP. Other modules only ask two things: `isStable()` and `getClient()`.

## Connection state machine

```mermaid
stateDiagram-v2
    [*] --> INIT
    INIT --> ETH_CONNECTING : immediately (fall-through)

    ETH_CONNECTING --> CONNECTED_ETH : link ON and IP != 0.0.0.0<br/>→ syncNTP()
    ETH_CONNECTING --> TRANSITION_DELAY : 10 s timeout<br/>next = WIFI_CONNECTING

    WIFI_CONNECTING --> CONNECTED_WIFI : WL_CONNECTED<br/>→ syncNTP()
    WIFI_CONNECTING --> TRANSITION_DELAY : 15 s timeout<br/>WiFi off, next = ETH_CONNECTING

    CONNECTED_ETH --> TRANSITION_DELAY : link lost<br/>SPI.end(), next = WIFI_CONNECTING
    CONNECTED_ETH --> CONNECTED_ETH : Ethernet.maintain() (DHCP lease)

    CONNECTED_WIFI --> TRANSITION_DELAY : Wi-Fi lost<br/>WiFi off, next = ETH_CONNECTING

    TRANSITION_DELAY --> ETH_CONNECTING : 2 s elapsed
    TRANSITION_DELAY --> WIFI_CONNECTING : 2 s elapsed

    note right of CONNECTED_ETH
        isStable() == true
        only in CONNECTED_ETH / CONNECTED_WIFI
    end note
```

Behaviour worth knowing:

- **No fail-back.** Once on Wi-Fi the reader stays on Wi-Fi until Wi-Fi drops, even if the Ethernet cable is plugged back in.
- **Blocking DHCP.** On entering `ETH_CONNECTING`, `Ethernet.begin(mac, 10000)` blocks up to 10 s while holding `networkClientMutex`. During that time the MQTT task cannot take the mutex, so new reads go straight to the outbox. That is safe, just slower.
- **Wi-Fi credentials are printed to the serial log** on every Wi-Fi attempt ([`network_manager.h:149`](../include/network_manager.h#L149)). Anyone with a USB cable can read the password.
- **Fixed MAC** `0A:8A:DA:11:42:01` ([`network_manager.h:34`](../include/network_manager.h#L34)) for Ethernet. Two readers on the same LAN segment will conflict — a blocker for multi-reader schools ([09 #5](09-known-issues-and-risks.md)).
- The task itself is **not** registered with the watchdog.

## Failover timeline

```plantuml
@startuml
title Ethernet cable unplugged, then Wi-Fi also unavailable
concise "NetworkManager" as NM
binary "isStable()" as ST
concise "MQTT" as MQ

@0
NM is CONNECTED_ETH
ST is high
MQ is connected

@5
NM is TRANSITION_DELAY
ST is low
MQ is "disconnect → outbox"

@7
NM is WIFI_CONNECTING

@22
NM is TRANSITION_DELAY

@24
NM is ETH_CONNECTING

@34
NM is TRANSITION_DELAY

@36
NM is WIFI_CONNECTING

@40
NM is CONNECTED_WIFI
ST is high
MQ is "reconnect → replay outbox"

@enduml
```

(Seconds. Cable pulled at t = 5 s; Wi-Fi AP comes back at t ≈ 40 s.)

## Mutex usage between network and MQTT

```mermaid
sequenceDiagram
    participant NT as Network Task (500 ms)
    participant MX as networkClientMutex
    participant MT as MQTT Task (50 ms)

    NT->>MX: take (10 ms)
    Note over NT: state machine tick<br/>(may block: DHCP ≤ 10 s, NTP ≤ 5 s)
    MT-->>MX: take (50 ms) — fails while NT holds it
    Note over MT: live publish skipped → record to outbox
    NT->>MX: give
    MT->>MX: take (10 ms)
    Note over MT: reconnect / mqtt.loop / outbox replay
    MT->>MX: give
```

The mutex prevents the MQTT task from using a `Client` while the network manager is tearing it down (e.g. `SPI.end()` on Ethernet loss). Because `getClient()` returns `EthernetClient` or `WiFiClient` depending on the active link, MQTT re-binds the client on every `reconnect()`.

## NTP

| Link | Method | Notes |
|---|---|---|
| Wi-Fi | `configTime(7200, 0, "pool.ntp.org")` + `getLocalTime(5 s)` | ESP-IDF SNTP; keeps re-syncing in the background |
| Ethernet (W5500) | Manual NTP request over `EthernetUDP` (local port 8888), `settimeofday()`, `TZ=STD-2` | One-shot at connect time; no periodic re-sync while the link stays up |

The W5500 has its own TCP/IP stack (not lwIP), so the IDF SNTP client cannot use it. That is why the Ethernet path builds the 48-byte NTP packet by hand. The Olimex variant uses the ESP32's internal MAC + lwIP, so `configTime()` works for both links there.

Requirements for the school network:

- UDP 123 outbound to `pool.ntp.org` must be allowed, or the NTP server must be changed to a local one (it is a compile-time constant).
- DHCP must be available on the Ethernet segment (no static IP support).
- TCP to the broker at `MQTT_SERVER:MQTT_PORT`.

## Olimex variant differences

[`network_manager_olimex.h`](../include/network_manager_olimex.h) keeps the same states and public API but:

- Uses `ETH.begin(...)` (LAN8720, RMII) and reacts to `ARDUINO_EVENT_ETH_GOT_IP` / `ETH_DISCONNECTED` events via a static `_instance` pointer.
- Needs `networkManager.setup()` to register those event handlers — `main.cpp` does not call it today.
- Returns a single `WiFiClient` for both links (lwIP serves both).
- Uses `configTime()` for NTP on both links.
