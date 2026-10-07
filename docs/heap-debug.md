# Heap corruption hunt (PR #29, mDNS)

Bench, PR #29 at `eaa2b1b`: two panics, identical backtrace —
`LoadProhibited, excvaddr 0x81da1e70`, `block_size ← tlsf_walk_pool ←
multi_heap_get_info ← heap_caps_get_largest_free_block(caps=4) ← handleSession`
(`GET /api/v1/session`). Boot 99→100 idle with only the hub polling; 100→101
during an mDNS IP-follow test. The session handler's heap walk is where an
already-corrupted internal heap was **found**, not what corrupted it.

This branch does not claim a fix. It adds:

| What | Where |
|---|---|
| Periodic integrity check: internal heap every 2 s, all heaps every 60 s; first failure logged with time, last good check and the last 4 mDNS actions; kept in RTC memory across a panic reboot | `heapdiag.cpp`, `CARDIAG_HEAP_CHECK` |
| Comprehensive heap poisoning (catches the bad write at the culprit's next malloc/free) | `esp32-can-x2-heapdebug*` envs, `custom_sdkconfig` |
| mDNS off at build time, resolver and advertisement separately | `CARDIAG_MDNS_RESOLVE`, `CARDIAG_MDNS_ADVERTISE` |
| `/api/v1/session` and the stats line no longer walk the heap per request: loop() samples every 5 s | `heapdiag.cpp`, `HEAP_SAMPLE_MS` |
| mdns task stack high-water mark (bytes free), sampled with the heap | `heapdiag.mdns_stack_free[_min]` |
| Query cancelled at link-down is parked and reaped, not leaked | `hubquery.h`, `test/test_hubquery` |

## Envs (identical source; only the flags differ)

| Env | mDNS resolve | mDNS advertise | Poisoning | mdns stack |
|---|---|---|---|---|
| `esp32-can-x2-heapdebug` | on | on | comprehensive | 4096 |
| `esp32-can-x2-heapdebug-nomdns` | off | off (no `mdns_init`) | comprehensive | — |
| `esp32-can-x2-heapdebug-noresolve` | off | on | comprehensive | 4096 |
| `esp32-can-x2-heapdebug-noadvertise` | on | off | comprehensive | 4096 |
| `esp32-can-x2-heapdebug-mdnsstack8k` | on | on | comprehensive | **8192** |
| `esp32-can-x2-heapcheck` (CI) | on | on | light (stock) | 4096 |

⚠ The `heapdebug*` envs set `custom_sdkconfig`, so pioarduino rebuilds the
Arduino libraries locally. The **first** build downloads ESP-IDF and takes a
long time; later builds are normal. CI builds `heapcheck` (same sources, stock
sdkconfig, and again with mDNS off), not these.

## Bench steps

1. **Build and flash** (from `firmware/`):

   ```
   pio run -e esp32-can-x2-heapdebug -t upload
   ```

2. **Reset with esptool, then capture with the safe tool** (CLAUDE.md: a default
   serial open can reset the board):

   ```
   python <platformio>/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port COM3 --after hard_reset read_mac
   python tools/serial_capture.py --port COM3 --seconds 43200 --out heapdebug-serial.log
   ```

   (`--seconds 43200` = 12 h; start it right after the esptool reset so the
   banner is in the file.) Expect in the banner:

   ```
   [heap] integrity check ON (every 2000 ms), mdns resolve=1 advertise=1
   [mdns] advertising cardiag.local + _cardiag._tcp port 80: ok
   ```

   and in `GET /api/v1/session`: `heapdiag.check.enabled: true`, `checks`
   climbing ~30/min, `mdns_build` matching the env.

3. **Hub as in the failing run.** The hub on `bench/hub-clock-mdns` with
   `network.bench.logger_ip: cardiag.local`, polling as usual: that is the
   idle case that panicked (99→100). For the **noadvertise** and **nomdns**
   envs the name does not resolve, so set the logger's IP instead
   (`logger_ip: 192.168.137.x`, `logger_http: http://192.168.137.x`).

4. **Watch it overnight** (PowerShell; `<ip>` = the logger):

   ```powershell
   while ($true) {
     try {
       $s = Invoke-RestMethod http://<ip>/api/v1/session -TimeoutSec 10
       $h = $s.heapdiag
       "$(Get-Date -f s) boot=$($s.boot_id) up=$($s.uptime_ms) checks=$($h.check.checks) " +
       "fail=$($h.check.failures) first_fail_ms=$($h.check.first_fail_ms) " +
       "max_check_us=$($h.check.max_check_us) stack_min=$($h.mdns_stack_free_min) " +
       "parked=$($h.mdns_parked)/$($h.mdns_reaped) prev=$($h.prev_boot_fail | ConvertTo-Json -Compress)"
     } catch { "$(Get-Date -f s) UNREACHABLE $($_.Exception.Message)" }
     Start-Sleep 300
   } | Tee-Object -FilePath heapwatch.log -Append
   ```

   Keep the serial capture running too: the heap code prints the corrupt
   block's address on the first failure, and comprehensive poisoning usually
   aborts at the culprit's malloc/free with its backtrace.

5. **Pass** (one env, one night): `boot_id` unchanged all night,
   `check.failures == 0`, `checks` ≈ hours × 1800, `prev_boot_fail: null`,
   `mdns_parked == mdns_reaped` (± one in flight), `mdns_stack_free_min` noted.
   Also do the IP-follow test (the 100→101 case) at least twice during the run.

6. **On a failure**, keep: the serial lines from `CORRUPT HEAP` /
   `[heap] INTEGRITY CHECK FAILED` / any `abort()` with backtrace, the
   `heapwatch.log` line, and the coredump (`GET /api/v1/coredump`) if it
   panicked. The next boot also prints `[heap] PREVIOUS BOOT …` and reports
   `heapdiag.prev_boot_fail`.

## Bisect order

1. `heapdebug` (everything on) — does it reproduce with the checks on?
2. If it does: `nomdns`. Clean ⇒ mDNS is involved; still corrupt ⇒ it is not
   mDNS, and PR #29 only moved the timing.
3. If mDNS is involved: `noresolve` (responder only) and `noadvertise`
   (querier only) — which half.
4. `mdnsstack8k` in parallel with 3 if `mdns_stack_free_min` came out low
   (< ~512 B) in step 1: the stack-overflow hypothesis, tested directly.
