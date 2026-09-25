<#
.SYNOPSIS
  200-cycle overnight Wi-Fi soak. Detached, resumable, leaves usable partial
  data if killed.

.DESCRIPTION
  Answers: does the board survive 200 AP<->STA transitions with a FLAT heap and
  ZERO reboots. Heap slope and min-free-heap are the leak test; the reboot
  count plus its CLASSIFICATION is the stability test.

  ⭐ NO CAN HARNESS. Confirmed from the source before writing this:
    * soak_wifi.py toggles the Windows hotspot and reads USB serial. It never
      touches the CAN bus.
    * MODE_SELFTEST is TWAI *internal* loopback -- config.h: "No wiring,
      nothing connected".
  The bench harness is dupont + tape and BROWNS OUT WHEN MOVED, so every
  capture taken on it is suspect. Since the soak does not need it, running
  USB-only REMOVES that confound rather than classifying it afterwards.

  🔌 BEFORE LAUNCHING, physically:
    1. DISCONNECT the dupont CAN harness from the board entirely.
    2. Plug the board into a DIRECT laptop USB port -- not a hub, which adds
       a second thing that can sag.
    3. TAPE THE CABLE DOWN so an overnight knock cannot brown it out.
  A BROWNOUT in the summary after doing all three is a real finding about the
  board or the cable, not about the harness.

.PARAMETER Cycles
  Default 200. At ~45 s/cycle (20 s dwell + transitions) that is ~2.5 h.

.PARAMETER Resume
  An existing analysis\soak-* folder to continue into. Cycle rows APPEND to the
  same CSV, so a soak cut at 180 can be topped up rather than restarted.

.PARAMETER SkipFlash
  Do not reflash. Only safe when the board already carries the
  `[boot] RESET REASON` line (cardiag 75e6266 or later) -- without it every
  reset classifies as UNREPORTED and the run cannot tell a brownout from a
  watchdog, which is half the point.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_soak.ps1
  powershell -ExecutionPolicy Bypass -File tools\run_soak.ps1 -Resume analysis\soak-2026-09-24-2300
#>
param(
  [int]$Cycles = 200,
  [string]$Port = "COM3",
  [double]$Dwell = 20.0,
  [string]$Env = "esp32-can-x2",
  [string]$Resume = "",
  [switch]$SkipFlash,
  [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

if ($Resume) {
  $out = if ([System.IO.Path]::IsPathRooted($Resume)) { $Resume }
         else { Join-Path $repo $Resume }
  if (-not (Test-Path $out)) { Write-Host "!! $out does not exist"; exit 2 }
} else {
  $stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
  # Rule 3: a fixed, dated folder in the repo's analysis dir. Never %TEMP%.
  $out = Join-Path $repo "analysis\soak-$stamp"
  New-Item -ItemType Directory -Force -Path $out | Out-Null
}

$pio = "C:\Users\turru\.platformio\penv\Scripts\pio.exe"
# `pio pkg exec -p tool-esptoolpy -- esptool` fails with WinError 2 on this
# install; call the script through PlatformIO's own interpreter instead.
# Used ONLY for a deterministic hard reset before the boot capture.
$pioPy   = "C:\Users\turru\.platformio\penv\Scripts\python.exe"
$esptool = "C:\Users\turru\.platformio\packages\tool-esptoolpy\esptool.py"
# Known trap: without this, pio upload dies after ~10 min on a cp1252
# UnicodeEncodeError that names nothing relevant.
$env:PYTHONIOENCODING = "utf-8"

function Log($m) {
  $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
  Write-Host $line
  Add-Content -Path (Join-Path $out "runner.log") -Value $line
}

# A DONE marker so a later session can tell "finished" from "killed at 3am"
# without reading the logs. Removed at start, written at every exit path.
$doneFile = Join-Path $out "DONE"
Remove-Item $doneFile -ErrorAction SilentlyContinue
function Finish($code, $state, $verdict = "") {
  Set-Content -Path $doneFile -Encoding utf8 -Value @(
    "state: $state",
    "verdict: $verdict",
    "exit_code: $code",
    "cycles_requested: $Cycles",
    "finished: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')",
    "output: $out"
  )
  Log "DONE ($state, exit $code) -> $doneFile"
  if (-not $NoPause) { Read-Host "press Enter" | Out-Null }
  exit $code
}

Log "=== cardiag 200-cycle Wi-Fi soak ==="
Log "output -> $out"
if ($Resume) { Log "RESUMING into an existing folder; CSV rows will append" }

# ⭐ CONTEXT FOR THE MORNING READ. Whoever opens SUMMARY.md at 8am did not
# watch this start. A BROWNOUT at cycle 140 means one thing on mains and a
# completely different thing if the laptop dropped to battery at 3am, so the
# power state is recorded as evidence, not as a note.
$bat = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
$acOnline = if ($bat) { [bool]($bat.BatteryStatus -eq 2) } else { $true }
$ctx = [ordered]@{
  started        = (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
  cycles         = $Cycles
  dwell_s        = $Dwell
  env            = $Env
  port           = $Port
  on_ac_power    = $acOnline
  battery_pct    = if ($bat) { $bat.EstimatedChargeRemaining } else { "n/a (desktop)" }
  sleep_ac       = "Never (set by this runner)"
  can_harness    = "DISCONNECTED (soak is USB-only; SELFTEST is internal loopback)"
  usb            = "direct laptop port, no hub, cable taped"
}
$ctx.GetEnumerator() | ForEach-Object { Log ("  {0,-14} {1}" -f $_.Key, $_.Value) }
$ctx | ConvertTo-Json | Set-Content -Path (Join-Path $out "run-context.json") -Encoding utf8
if (-not $acOnline) {
  Log "!! LAPTOP IS ON BATTERY. An overnight soak will die or brown out."
  Log "   Plug it in and relaunch. ABORTING."
  Finish 12 "preflight-battery"
}

# --- PRE-FLIGHT -------------------------------------------------------------
Log "--- pre-flight ---"

$sleepAC = (powercfg /query SCHEME_CURRENT SUB_SLEEP STANDBYIDLE |
            Select-String 'Current AC Power Setting Index').ToString().Split(':')[-1].Trim()
if ($sleepAC -ne '0x00000000') {
  Log "sleep on AC is $sleepAC, not Never. Setting it."
  powercfg /change standby-timeout-ac 0
}
# An overnight run also must not blank into hibernate.
powercfg /change hibernate-timeout-ac 0
powercfg /change monitor-timeout-ac 0
Log "sleep/hibernate on AC: Never"

$free = [math]::Round((Get-PSDrive C).Free / 1GB, 1)
Log "disk free: $free GB"
if ($free -lt 5) { Log "!! under 5 GB free. STOP and clear space."; Finish 3 "preflight-disk" }

if (-not ([System.IO.Ports.SerialPort]::GetPortNames() -contains $Port)) {
  Log "!! $Port not found. Is the board plugged in? ABORTING."
  Finish 2 "preflight-port"
}
Log "$Port present"

# The soak itself toggles the hotspot, but it must exist and be ON to start.
$hs = & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hotspot.ps1') -Action state
if ($hs -notmatch 'On') {
  Log "hotspot off -> starting"
  & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hotspot.ps1') -Action start | Out-Null
  Start-Sleep -Seconds 3
}
Log "hotspot: on"

# Same gate as run_bench.ps1: a -D flag a header quietly overrides means the
# run measures something other than what it says.
Log "--- config guard check ---"
& python (Join-Path $PSScriptRoot 'check_config_guards.py') 2>&1 |
  Tee-Object -FilePath (Join-Path $out "guards.log") -Append |
  ForEach-Object { Log "  $_" }
if ($LASTEXITCODE -ne 0) { Log "!! unguarded overridable macro(s). ABORTING."; Finish 7 "preflight-guards" }

# --- FLASH ------------------------------------------------------------------
# The reset-reason line is what separates BROWNOUT from TASK_WDT. Without it
# every reset lands in UNREPORTED and the run cannot answer its own question.
if ($SkipFlash) {
  Log "SkipFlash set -- assuming the board already prints [boot] RESET REASON"
} else {
  Log "--- flashing $Env (for [boot] RESET REASON) ---"
  Push-Location (Join-Path $repo 'firmware')
  & $pio run -e $Env -t upload --upload-port $Port 2>&1 |
    Tee-Object -FilePath (Join-Path $out "flash.log") -Append | Out-Null
  $rc = $LASTEXITCODE
  Pop-Location
  if ($rc -ne 0) { Log "!! flash FAILED (see flash.log). ABORTING."; Finish 4 "flash" }
  Log "flashed"
}

# --- 3b. ENTER SELFTEST, AND PROVE IT ---------------------------------------
# ⚠️ SELFTEST DOES NOT SURVIVE A BOOT, AND THAT IS CORRECT. main.cpp refuses to
# resume a transmitting mode unattended and forces MODE_LISTEN -- the right
# guard for a board that wakes up plugged into a car. So the keys have to be
# sent every time: '3' selects SELFTEST, 'y' confirms within the window.
#
# 🔑 And then it is VERIFIED, not assumed. Without this the soak would happily
# run 200 cycles of Wi-Fi churn in LISTEN with no CAN traffic at all, report a
# clean PASS, and answer a question nobody asked.
#
# ⚠️ `--reset` IS LOAD-BEARING, NOT TIDINESS. setup() prints the banner --
# RESET REASON, EFFECTIVE CAPS, the mount line -- about 2 s after boot, and
# esptool resets the board the moment the upload finishes. Opening the port a
# few seconds later therefore starts reading PAST the only lines the caps gate
# can check, and the gate correctly aborts on evidence it never had a chance to
# see. That happened on the 22:45 attempt. Resetting AFTER the port is already
# open removes the race instead of timing around it.
Log "--- entering SELFTEST + verifying ---"
$boot = Join-Path $out "boot.log"
$bootText = ""
for ($try = 1; $try -le 3; $try++) {
  # 🔑 esptool's hard reset, THEN capture. Measured on this board 2026-09-24:
  # serial_capture's --reset (DTR/RTS pulse) does NOT reset it -- uptime kept
  # climbing through 122 s -- and neither does closing the port, despite the
  # standing note that it does. esptool's `--after hard_reset` provably does.
  # Without a deterministic reset this is a ~2 s race against setup()'s
  # delay(2000), which the 22:45 attempt lost and correctly aborted on.
  Log "  attempt $try -- hard reset via esptool, then capture"
  & $pioPy $esptool --chip esp32s3 --port $Port --after hard_reset read_mac 2>&1 |
    Out-File -FilePath (Join-Path $out "reset.log") -Append -Encoding utf8
  & python (Join-Path $PSScriptRoot 'serial_capture.py') `
      '--port' $Port '--seconds' '45' '--delay' '20' '--gap' '3' `
      '--send' '3' '--send' 'y' '--out' $boot '--quiet' 2>&1 | Out-Null
  if (Test-Path $boot) {
    $bootText = Get-Content $boot -Raw
    if ($bootText -match 'EFFECTIVE CAPS') { Log "  boot banner captured"; break }
  }
  Log "  no boot banner this attempt; retrying"
}
if (-not $bootText) { Log "!! no serial captured. ABORTING."; Finish 9 "selftest-noserial" }
if ($bootText -notmatch 'EFFECTIVE CAPS') {
  Log "!! never captured the boot banner in 3 attempts. ABORTING rather than"
  Log "   soaking all night without knowing which caps the board carries."
  Finish 9 "selftest-nobanner"
}

# (a) production caps, NOT the 10% bench cap. Script-checked, not eyeballed.
Log "--- asserting PRODUCTION caps (tier A 40%) ---"
& python (Join-Path $PSScriptRoot 'assert_effective_caps.py') `
    '--env' $Env '--serial' $boot '--expect' 'FS_TIER_A_MAX_PCT=40' 2>&1 |
  Tee-Object -FilePath (Join-Path $out "caps.log") -Append |
  ForEach-Object { Log "  $_" }
if ($LASTEXITCODE -ne 0) {
  Log "!! caps are NOT production defaults -- this board may still carry the"
  Log "   10% bench cap. ABORTING (see caps.log)."
  Finish 8 "caps-mismatch"
}

# (b) did it actually enter SELFTEST?
if ($bootText -match 'Mode:\s*SELFTEST') {
  Log "SELFTEST confirmed ('Mode: SELFTEST' in boot.log)"
} else {
  $m = [regex]::Matches($bootText, 'Mode:\s*(\w+)')
  $last = if ($m.Count) { $m[$m.Count - 1].Groups[1].Value } else { "<none seen>" }
  Log "!! board is in '$last', NOT SELFTEST. The 'y' confirm may have missed"
  Log "   its window. ABORTING -- 200 cycles with no CAN traffic answers"
  Log "   nothing. See boot.log."
  Finish 10 "selftest-not-entered"
}

# (c) is CAN traffic actually flowing? SELFTEST drives TWAI internal loopback,
# so frames appear on serial. Mode set but no frames = nothing being exercised.
$frames = ([regex]::Matches($bootText, 'STD 0x[0-9A-Fa-f]{3}')).Count
if ($frames -lt 10) {
  Log "!! only $frames CAN frame(s) seen on serial -- SELFTEST is not driving"
  Log "   the bus. ABORTING. See boot.log."
  Finish 11 "selftest-no-traffic"
}
Log "CAN traffic confirmed: $frames frame(s) in the boot window"

# --- THE SOAK ---------------------------------------------------------------
# soak_wifi.py writes and fsyncs its CSV EVERY cycle, so a kill at 3am leaves
# every completed cycle on disk.
Log "--- soak: $Cycles cycles, dwell ${Dwell}s ---"
Log "    per-cycle CSV flushes as it goes; safe to kill"
Log "    raw serial -> serial-raw.log, every line host-timestamped and fsynced"
$csv = Join-Path $out "soak.csv"
& python (Join-Path $PSScriptRoot 'soak_wifi.py') `
    '--port' $Port '--cycles' $Cycles '--dwell' $Dwell '--csv' $csv `
    '--raw-log' (Join-Path $out "serial-raw.log") 2>&1 |
  Tee-Object -FilePath (Join-Path $out "soak.log") -Append |
  ForEach-Object { Write-Host $_ }
$soakRc = $LASTEXITCODE

# --- SUMMARY ----------------------------------------------------------------
# Written from the CSV, so a PARTIAL run still gets a real summary.
Log "--- writing SUMMARY.md ---"
# ⭐ ONE VERDICT. soak_summary.py decides it (judge()), writes it to
# verdict.json, and DONE and the end of soak.log are copied from that file.
# $soakRc is NOT consulted for pass/fail: soak_wifi.py's exit code comes from
# the same judge(), and reading two sources is how the soak once reported PASS
# on runs that had failed. A missing verdict.json is a failure, not a pass.
$verdictFile = Join-Path $out "verdict.json"
Remove-Item $verdictFile -ErrorAction SilentlyContinue
& python (Join-Path $PSScriptRoot 'soak_summary.py') `
    '--csv' $csv '--log' (Join-Path $out "soak.log") `
    '--out' (Join-Path $out "SUMMARY.md") '--requested' $Cycles `
    '--context' (Join-Path $out "run-context.json") `
    '--verdict-file' $verdictFile 2>&1 |
  ForEach-Object { Log "  $_" }

$verdict = "UNKNOWN"
if (Test-Path $verdictFile) {
  try { $verdict = (Get-Content $verdictFile -Raw | ConvertFrom-Json).verdict } catch { }
}
Add-Content -Path (Join-Path $out "soak.log") -Encoding utf8 `
  -Value "FINAL VERDICT (soak_summary.py, same as SUMMARY.md and DONE): $verdict"

Log "read results from: $out"
switch ($verdict) {
  "PASS"    { Finish 0 "complete-pass" $verdict }
  "PARTIAL" { Finish 3 "partial" $verdict }
  # A required metric unreadable on >= 10 % of cycles: not a PASS.
  "INCONCLUSIVE" { Finish 4 "inconclusive" $verdict }
  "FAIL"    { Finish 1 "complete-fail" $verdict }
  default   { Finish 8 "summary-missing" $verdict }
}
