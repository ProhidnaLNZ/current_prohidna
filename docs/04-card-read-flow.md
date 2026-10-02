# 04 — Card read flow

Everything in this document happens in the **NFC Reader Task** (core 1) inside [`Nfc::read()`](../include/nfc.h#L87), except the final publish which the MQTT task does asynchronously.

## End-to-end sequence

```mermaid
sequenceDiagram
    autonumber
    actor P as Person
    participant PN as PN532
    participant N as Nfc (NFC task)
    participant W as Whitelist
    participant L as Led / Buzzer
    participant Q as readQueue
    participant M as MqttClient (MQTT task)
    participant B as Mosquitto
    participant BE as Python worker

    loop every ~10 ms
        N->>PN: readPassiveTargetID(ISO14443A, timeout 50 ms)
    end
    P->>PN: taps card
    PN-->>N: UID bytes
    N->>N: uid = "4-123-55-201"
    N->>N: debounce: new UID or > 2 s since last?
    N->>W: isReady()? isAllowed(uid)?
    W-->>N: allowed / denied
    N->>L: approval(700) + rising melody<br/>or denial(350) + falling melody
    opt 4-byte UID (MIFARE Classic)
        N->>PN: authenticate block 4 with key A = FF..FF
        N->>PN: read block 4 → first 8 bytes = legacy code
    end
    N->>N: record = uid:code:millis:event_id:allowed
    N->>Q: xQueueSend (wait ≤ 100 ms)
    Note over N,L: Person already saw/heard the result.<br/>Everything below is asynchronous.
    M->>Q: xQueueReceive (non-blocking, every 50 ms)
    alt network stable and MQTT connected
        M->>B: PUBLISH device/reader_real/events/read (QoS 0)
        B->>BE: deliver
    else offline or publish failed
        M->>M: DataBase.appendRead(record) → /outbox.txt
    end
```

## Decision logic

```mermaid
flowchart TD
    A([Nfc::read]) --> B{is_connected_?}
    B -- no --> C[tryConnect<br/>LED = loading] --> Z([return])
    B -- yes --> D[healthCheck every 2.5 s<br/>firmware version == 0 → disconnected]
    D --> E{card present?<br/>50 ms timeout}
    E -- no --> Z
    E -- yes --> F[uid string = decimal bytes joined by '-']
    F --> G{different UID than last<br/>OR > 2000 ms since last read?}
    G -- no --> Z
    G -- yes --> H{whitelist READY?}
    H -- no --> I[allowed = false<br/>log 'Whitelist not ready']
    H -- yes --> J{uid in whitelist?}
    J -- yes --> K[allowed = true]
    J -- no --> I
    K --> L1[LED green 700 ms<br/>approval melody]
    I --> L2[LED red 350 ms<br/>denial melody]
    L1 --> M[remember uid + time]
    L2 --> M
    M --> N{uidLength == 4?}
    N -- yes --> O[auth block 4 key A FF..FF<br/>read 8 bytes → legacy code]
    N -- no --> P[legacy code = 0]
    O --> P2[build record]
    P --> P2
    P2 --> Q[xQueueSend readQueue<br/>drop + log if full after 100 ms]
    Q --> Z
```

Key points:

- **Fail-closed:** if the whitelist is not `READY` (never synced and no cache, or a sync is in progress) every card is **denied** — and the denial is reported to the backend as `allowed=0`.
- **Debounce** ([`CARD_READ_COOLDOWN_MS = 2000`](../include/nfc.h#L14)): holding the same card on the reader produces one event every 2 s; a different card is processed immediately.
- Feedback is given **before** the legacy-code read and the queue push, so the person never waits for flash or network.
- `Buzzer::playApproval()/playDenial()` are ignored while a melody is still playing, so two fast taps give two light flashes but only one melody.

## Record format

Built in [`nfc.h`](../include/nfc.h#L170) as a colon-separated string:

```
<uid>:<legacy_code>:<millis>:<event_id>:<allowed>
```

| Field | Example | Description |
|---|---|---|
| `uid` | `4-123-55-201` | Card UID bytes in **decimal**, `-` separated. 4 bytes (Classic) or 7 bytes (Ultralight/NTAG/DESFire). |
| `legacy_code` | `0` / `1234567890123` | Unsigned 64-bit from block 4, decimal; `0` if not a 4-byte card or auth/read failed. |
| `millis` | `5234012` | `millis()` at read time (uptime). **Replaced with `HH-MM-SS` just before publishing** — see [05](05-storage-outbox-and-time.md#timestamp-handling). |
| `event_id` | `3f9a1c0e5b7d2a41` | 16 hex chars from `esp_random()`; idempotency key. |
| `allowed` | `1` / `0` | Local decision. |

Length budget: the queue item is **64 bytes including the terminating NUL**. A 4-byte UID with a large legacy code can exceed 63 characters (15 + 20 + 8 + 16 + 1 + 4 separators = 64), which overflows the item — [09 #2](09-known-issues-and-risks.md).

## Activity view (PlantUML)

```plantuml
@startuml
title Nfc::read() — one iteration
start
if (PN532 connected?) then (no)
  :tryConnect();
  :LED = loading;
  stop
endif
:healthCheck() (every 2.5 s);
if (card in field within 50 ms?) then (no)
  stop
endif
:uid = toDecimalDashString(uid bytes);
if (new uid OR cooldown 2 s passed?) then (no)
  stop
endif
if (whitelist READY and uid allowed?) then (yes)
  :LED green 700 ms;
  :play approval melody;
else (no)
  :LED red 350 ms;
  :play denial melody;
endif
if (4-byte UID?) then (yes)
  :authenticate block 4 (key A FF..FF);
  :read block 4 → legacy code;
endif
:event_id = generateUUID();
:record = uid:code:millis:event_id:allowed;
:xQueueSend(readQueue, 100 ms);
stop
@enduml
```

## Feedback timing

```plantuml
@startuml
title Feedback timeline for one allowed tap
robust "LED ring" as LED
robust "Buzzer" as BZ
concise "NFC task" as NFC

@0
LED is Waiting
BZ is Idle
NFC is Polling

@100
NFC is "Decide+feedback"
LED is Green
BZ is C4

@130
NFC is "Block4+enqueue"

@197
BZ is E4

@200
NFC is Polling

@294
BZ is G4

@391
BZ is C5

@488
BZ is Idle

@800
LED is Waiting
@enduml
```

Times are in ms and approximate: each note lasts `300 / 4 = 75 ms` followed by a gap so the next note starts ~97 ms later; the green fill lasts `700 / 16` ≈ 43 frames.

## LED animation state

```mermaid
stateDiagram-v2
    [*] --> loading : boot (default)
    loading --> waiting : PN532 connected (tryConnect OK)
    waiting --> loading : health check failed
    waiting --> Approval : approval(700)
    waiting --> Denial : denial(350)
    Approval --> waiting : timer expired
    Denial --> waiting : timer expired
    Approval --> Denial : new denied tap
    Denial --> Approval : new allowed tap
    note right of loading
        "error" (solid red) exists in the enum
        but nothing ever sets it
    end note
```

Approval/denial are overlays (timers) drawn on top of the base `animation`; when the timer reaches zero the base animation shows again.
