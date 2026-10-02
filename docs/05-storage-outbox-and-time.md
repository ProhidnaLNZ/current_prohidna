# 05 — Storage, outbox & time

## Flash layout (LittleFS)

| File | Owner | Format | Lifetime |
|---|---|---|---|
| `/outbox.txt` | `DataBase` ([`src/db.cpp`](../src/db.cpp)) | One read record per line (`uid:code:millis:event_id:allowed`) | Grows while offline, shrinks after successful replay |
| `/outbox.tmp` | `DataBase` | Temporary copy during cleanup | Renamed over `/outbox.txt` |
| `/whitelist.txt` | `Whitelist` ([`include/whitelist.h`](../include/whitelist.h)) | One allowed UID per line | Rewritten on each successful whitelist sync |

LittleFS is mounted by `DataBase::setup()` with `format = false`; only if mounting fails is the partition formatted (this wipes both files). A full flash erase (`pio run -t erase`) also wipes them.

## Store-and-forward outbox

The reader publishes **live** when it can, and falls back to the outbox when it cannot. A separate replay step drains the outbox every 5 s while connected.

```mermaid
flowchart TD
    subgraph Step1["MqttClient::process() — step 1: live path"]
        A[xQueueReceive readQueue] -->|item| B{take networkClientMutex ≤ 50 ms<br/>AND network stable<br/>AND MQTT connected?}
        B -- yes --> C[publish with time substituted]
        C -->|ok| D([done])
        C -->|fail| E
        B -- no --> E[DataBase.appendRead → /outbox.txt]
    end
    subgraph Step2["step 2: connection + replay"]
        F{take mutex ≤ 10 ms} -- yes --> G{network stable?}
        G -- no --> H[disconnect MQTT if connected]
        G -- yes --> I{MQTT connected?}
        I -- no --> J[reconnect]
        I -- yes --> K[mqtt.loop]
        J --> L{connected AND 5 s since last replay?}
        K --> L
        L -- yes --> M[processOutbox]
    end
    subgraph Replay["processOutbox()"]
        M --> N[getAllReads → vector of lines]
        N --> O{for each line}
        O --> P[publish, wait 50 ms]
        P -->|ok| Q[add to sent list] --> O
        P -->|fail or disconnected| R[stop]
        O -->|end| R
        R --> S[removeSentReads: rewrite file without sent lines]
    end
```

### Record lifecycle

```mermaid
stateDiagram-v2
    [*] --> InQueue : Nfc pushes record
    InQueue --> Dropped : queue full for 100 ms
    InQueue --> PublishedLive : online, publish OK
    InQueue --> InOutbox : offline / mutex busy / publish failed
    InOutbox --> InFlight : replay (every 5 s when connected)
    InFlight --> InOutbox : publish failed / disconnected
    InFlight --> PublishedReplay : publish OK
    PublishedReplay --> Removed : removeSentReads()
    PublishedReplay --> InOutbox : reboot / WDT reset before removal (→ duplicate later)
    PublishedLive --> [*]
    Removed --> [*]
    Dropped --> [*]
```

### Delivery guarantees (what the backend can rely on)

| Property | Value | Why |
|---|---|---|
| Survives reboot / power loss | Yes, once written to `/outbox.txt` | LittleFS append + `close()` |
| Lost in RAM | Up to 50 queued records on reboot; records dropped when the queue is full | `readQueue` is RAM only |
| Duplicates | **Possible** — replayed records are removed only after the whole batch | Backend must dedupe on `event_id` |
| Order | **Not guaranteed** — a live read can be published before older outbox records | Backend must not assume arrival order |
| Network delivery | QoS 0 ("at most once" per publish attempt) | `PubSubClient::publish()` is QoS 0; `-D MQTT_QOS=1` is unused |

### DataBase implementation notes

- All methods lock `_fileMutex` (a FreeRTOS mutex), so the outbox is safe even if more tasks start using it.
- `removeSentReads()` copies every line that is **not** in the sent list into `/outbox.tmp`, then deletes the original and renames. Complexity O(lines × sent). A power cut between `remove` and `rename` loses the outbox; LittleFS has no atomic replace for this.
- `getAllReads()` loads the **whole outbox into RAM** as `std::vector<String>`. A long outage (thousands of reads) can exhaust the heap — [09 #9](09-known-issues-and-risks.md).

## Sequence: replay after an outage

```plantuml
@startuml
title Outbox replay after reconnect
participant "MQTT task" as M
participant "DataBase" as D
database "/outbox.txt" as F
participant "Mosquitto" as B

== offline ==
M -> D : appendRead(r1)
D -> F : append "r1"
M -> D : appendRead(r2)
D -> F : append "r2"

== network back, reconnect OK ==
M -> D : getAllReads()
D -> F : read all lines
D --> M : [r1, r2]
loop for each record
  M -> M : replaceMillisWithTime(r)
  M -> B : PUBLISH events/read (QoS 0)
  M -> M : vTaskDelay(50 ms)
end
M -> D : removeSentReads([r1, r2])
D -> F : copy unsent lines to /outbox.tmp
D -> F : remove /outbox.txt, rename tmp
note over M
  The whole batch runs inside one process() call
  without esp_task_wdt_reset(): ~600 records ≈ 30 s
  → watchdog reboot before removeSentReads().
end note
@enduml
```

## Timestamp handling

The reader has no RTC. Time comes from NTP after a network connection (see [06](06-networking.md#ntp)). Records store **uptime** (`millis()`) and are converted to wall-clock time **at publish time** by [`replaceMillisWithTime()`](../include/timer.h):

```mermaid
flowchart LR
    A["record<br/>4-1-2-3:0:<b>5234012</b>:ab..:1"] --> B["take 3rd field<br/>eventMillis = 5234012"]
    B --> C{"first call after<br/>time() > 2023-11-14?"}
    C -- "not yet synced" --> D["'00-00-00'"]
    C -- "synced (once per boot)" --> E["remember ntpSyncMillis, ntpSyncTime"]
    E --> F["eventTime = ntpSyncTime + (eventMillis − ntpSyncMillis)/1000"]
    F --> G["localtime → 'HH-MM-SS'"]
    D --> H["payload<br/>4-1-2-3:0:<b>08-15-42</b>:ab..:1"]
    G --> H
```

Consequences the backend must know:

1. **No date** in the payload — only `HH-MM-SS`. The backend has to assume the date from the receive time. Records replayed after midnight get the wrong day.
2. **Reboot breaks old records:** `millis()` restarts at 0, but outbox lines keep the previous boot's uptime. After a reboot those records are converted against the *new* boot's sync point → wrong time.
3. **`00-00-00`** is sent if the clock has never been synced in this boot (e.g. published before NTP succeeded).
4. **Time zone** is a fixed UTC+2 without daylight saving (`GMT_OFFSET_SEC = 7200`, `DAYLIGHT_OFFSET_SEC = 0`). During Eastern European Summer Time (UTC+3) times are one hour behind local time.
5. The sync point is captured lazily on first conversion; the backend cannot tell from the payload whether the time is trustworthy.

Recommended fix (see [10](10-roadmap-and-open-questions.md)): store an absolute epoch timestamp when the time is known, otherwise store `millis` + a boot id, and send ISO 8601 UTC.
