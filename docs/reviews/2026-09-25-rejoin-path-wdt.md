# Review — blocking calls on the STA rejoin path under the task watchdog

**2026-09-25. Review only; nothing here is fixed.** Line numbers are against
`master` at `bd8f5b2`.

**Trigger.** The 200-cycle Wi-Fi soak had one `TASK_WDT` reset, at cycle 135,
right after a failed STA rejoin. Its coredump was lost, because the firmware
erases the dump at boot after printing a summary. So this review works from the
code alone and cannot name the call that stalled. It lists every candidate, with
the strength of the evidence for each.

## What the watchdog actually watches

- **One task: `loop()`.** It is subscribed at `firmware/src/main.cpp:667`
  (`esp_task_wdt_add(nullptr)` in `setup()`), with `idle_core_mask = 0` and
  `WDT_TIMEOUT_S = 30` (`config.h`). It is fed once per pass, at
  `main.cpp:810`.
- **Not watched:** `canTask`, the Wi-Fi and event tasks, and the idle tasks. A
  hang in any of those can never produce a `TASK_WDT`. ⚠️ A separate gap: a
  hung `canTask` is invisible to the watchdog.
- **So a `TASK_WDT` means one `loop()` pass took more than 30 s.** Everything
  below is something a single pass calls. In the order the pass calls it:
  `webuiLoop()` → `hublinkLoop()` → `hubstreamLoop()` → `filestoreLoop()`
  (`main.cpp:820-823`).

## The failed-rejoin pass, call by call

In AP mode, `hublinkLoop()` retries the hub every `WIFI_STA_RETRY_MS` (60 s),
or after `WIFI_STA_QUICK_RETRY_MS` (10 s) once following a drop. A **failed**
retry runs all of the following inside **one** `loop()` pass, with no feed:

| # | Call | Where | Bounded by | Why it could approach or exceed 30 s |
|---|---|---|---|---|
| 1 | `webuiStop()` → `g_server.stop()`, `WiFi.softAPdisconnect(true)`, `WiFi.mode(WIFI_OFF)` | `hublink.cpp:197` → `webui.cpp:295-298` | **the core only** — nothing in this repo bounds it | Stops the AP and the Wi-Fi driver. No timeout here. INFERRED: a driver stop that waits on an event can take as long as the core lets it |
| 2 | `WiFi.mode(WIFI_STA)`, `WiFi.begin()` | `hublink.cpp:88`, `:91` | the core only | Restarts the driver in STA. **This is the transition that logged `netstack cb reg failed with 12308`** in earlier soaks. It recovered then, but a netif registration failure is exactly the kind of fault where a following wait runs to its own timeout |
| 3 | join poll: `while (millis() - t0 < timeoutMs) { … delay(50); }` | `hublink.cpp:96-108` | **8 s** (`WIFI_STA_TIMEOUT_MS`) — CONFIRMED by code | The largest *known* block. Cannot reach 30 s alone; it is the biggest term in the sum |
| 4 | `WiFi.disconnect(true)`, `WiFi.mode(WIFI_OFF)` | `hublink.cpp:112-113` | the core only | A second driver stop in the same pass, straight after a failed association |
| 5 | `webuiStart()` → `WiFi.mode(WIFI_AP)`, `WiFi.softAP(...)` | `hublink.cpp:203` → `webui.cpp:261-262` | the core only | A third driver start. `softAP()` returns only once the AP is up or has failed. If the netstack is in the state that produced 12308, this is where a wait would stall |

**Four driver mode transitions plus up to 8 s of polling, all in one unfed pass.**
Only term 3 is bounded by this firmware. Terms 1, 2, 4 and 5 are bounded, if at
all, by timeouts inside the Arduino core / ESP-IDF, and this review could not
read those here: the platform package isn't available in this environment. ⇒
**The prime suspect for cycle 135 is this chain, most likely term 2 or term 5.**
That's INFERRED from the timing ("after a failed STA rejoin") and from where
12308 was seen before. Nothing in hand confirms it.

The drop path is the same shape, but shorter: `fallBackToAp()` calls
`webuiStop()` then `webuiStart()` (`hublink.cpp:133`, `:135`). That is two
transitions, with no join poll.

## Other blocking calls in the watched task

These are not on the rejoin path, but a single `loop()` pass can reach them.

| # | Call | Where | Bounded by | Risk |
|---|---|---|---|---|
| 6 | `streamCsv()`: `while (chunk…) g_server.sendContent(buf, n);` | `webui.cpp:191-193` | **nothing** — no watchdog feed, and no cap on total time | `/api/raw.csv` streams the whole PSRAM ring (~4 MB). Over a marginal link, or to a peer that stops reading, this passes 30 s on throughput alone. Every `sendContent()` blocks on the socket. **Real, but not the soak's cause**: the soak makes no HTTP requests (`soak_wifi.py` and `run_soak.ps1` do serial and hotspot control only) |
| 7 | `handleList()` loop of `sendContent()` | `filestore.cpp:938-980` | ≤ 96 entries (`FS_MAX_FILES`); no feed | A peer that stalls mid-listing blocks each write. Bounded in count, not in time |
| 8 | `handleFetch()` send loop | `filestore.cpp:1051-1069` | feeds after each chunk sent | A stall *inside* one `sendContent()` still trips the watchdog, as the comment there intends |
| 9 | `WebServer::handleClient()` reading a request | via `webuiLoop()`, `main.cpp:820` | the core's HTTP read/send timeouts (INFERRED: a few seconds each) | A client that opens a connection and trickles the request. Bounded by the core, not by this repo |
| 10 | `lockTable()` / `lockRec()`: `xSemaphoreTake(…, portMAX_DELAY)` | `sniffer.cpp:54`, `recorder.cpp:41` | as long as `canTask` holds the lock | `canTask` holds it only for short updates, and the FreeRTOS mutex inherits priority. **Low** unless `canTask` itself wedges while holding it, which (see above) the watchdog would never report as `canTask` |
| 11 | `emitUdp()`: `beginPacket` / `endPacket` | `hubstream.cpp` `emitUdp()` | lwIP; normally does not block for long | Runs only in STA. A send right after the link drops fails fast in the usual case. **Low** |
| 12 | POLL mode: `obdRequest()` wait, and `delay(OBD_INTER_REQUEST_MS)` per PID | `obd.cpp:119`, `main.cpp:710` | response timeout × PID count | POLL only, so not in the soak. Bounded, but the sum over a long PID list should be checked against 30 s once the BMW PID list exists |
| 13 | `canPause()` | `main.cpp:220` | 3 × `CAN_RX_WAIT_MS` | Bounded and small |

## What would turn "most likely" into "this line"

1. **Keep the coredump** until it's fetched (cardiag item 2, a separate PR). The
   dump names the task and the PC of the stall, which ends the guessing.
2. **Timestamp each Wi-Fi call** on the rejoin path. Add a `[hublink] t+NNNNms <call>`
   line before and after terms 1, 2, 4 and 5. The last line printed before a
   `TASK_WDT` then names the call. Raw serial per line with host timestamps
   (item 5) keeps the evidence even if the board resets mid-line.
3. **Count 12308 per transition type** in the soak summary. If the watchdog
   resets only follow a 12308, that's the link.

## Fix directions, for after the evidence (not done here)

- Split the failed-rejoin chain across `loop()` passes. A small state machine
  (stop AP → start STA → poll → stop STA → start AP) would put one driver
  transition in each pass and feed the watchdog between them. That puts a bound
  on every pass without touching the timeout.
- Give `streamCsv()` a feed after each chunk it sends, and a total cap,
  following `handleFetch()`.
- Subscribe `canTask` to the watchdog, or give it its own liveness check, so a
  hang there becomes visible.
