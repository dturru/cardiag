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
function Finish($code, $state) {
  Set-Content -Path $doneFile -Encoding utf8 -Value @(
    "state: $state",
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
  Start-Sleep -Seconds 3
}

# --- THE SOAK ---------------------------------------------------------------
# soak_wifi.py writes and fsyncs its CSV EVERY cycle, so a kill at 3am leaves
# every completed cycle on disk.
Log "--- soak: $Cycles cycles, dwell ${Dwell}s ---"
Log "    per-cycle CSV flushes as it goes; safe to kill"
$csv = Join-Path $out "soak.csv"
& python (Join-Path $PSScriptRoot 'soak_wifi.py') `
    '--port' $Port '--cycles' $Cycles '--dwell' $Dwell '--csv' $csv 2>&1 |
  Tee-Object -FilePath (Join-Path $out "soak.log") -Append |
  ForEach-Object { Write-Host $_ }
$soakRc = $LASTEXITCODE

# --- SUMMARY ----------------------------------------------------------------
# Written from the CSV, so a PARTIAL run still gets a real summary.
Log "--- writing SUMMARY.md ---"
& python (Join-Path $PSScriptRoot 'soak_summary.py') `
    '--csv' $csv '--log' (Join-Path $out "soak.log") `
    '--out' (Join-Path $out "SUMMARY.md") '--requested' $Cycles 2>&1 |
  ForEach-Object { Log "  $_" }

Log "read results from: $out"
if ($soakRc -eq 0) { Finish 0 "complete-pass" } else { Finish $soakRc "complete-fail" }
