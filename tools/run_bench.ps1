<#
.SYNOPSIS
  Clean-partition retention bench run. Detached, resumable, leaves usable
  partial data if killed.

.DESCRIPTION
  Answers the two questions runs 1-3 could not reach, plus the one they raised:

    CLAIM 2  enforceTierACap() picks ACKED Tier A before unacked -- the cap's
             OWN choice, not the global evictOne() order that was already
             verified. Needs Tier A to actually reach its 40% cap, which only
             happens on an empty partition.
    CLAIM 4  warn fires on unrecorded loss while TOTAL usage is still under
             warn_pct. Runs 1-3 sat at 87-91%, so usage carried the warning by
             itself and the second arm was never under test.
    REBOOT   the 2026-09-24 run rebooted mid-flight and the cause was never
             established. Serial is captured for the WHOLE window here, so the
             boot banner and its reset reason land in serial.log.
             ** This is the higher-priority question of the two. **
    CLAIM 5  the NVS lifetime loss record survives erasing the spiffs
             partition -- the closest thing to a format this device does.

  WHY A CLEAN PARTITION. The bench flash is Tier B dominant (~3.1 MB of 4.06)
  and sits at 87-91%. Tier A never exceeds ~70 kB against a ~1.6 MB cap, so
  enforceTierACap() is never entered at all, and usage never drops below the
  70% warn threshold. Erasing spiffs fixes both in one move.

  NVS IS DELIBERATELY NOT ERASED. It holds the lifetime loss counters, and
  leaving it is what makes claim 5 testable.

.PARAMETER Minutes
  Watch duration. Default 30. Tier A needs time to climb to its cap.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_bench.ps1
#>
param(
  [int]$Minutes = 30,
  [string]$Port = "COM3",
  [int]$AckAfter = 6,
  # Which build to flash. captest leaves the snapshot period at its 1000 ms
  # default so TIER A wins the race to the disk -- see platformio.ini. fstest
  # accelerates Tier B and is the wrong build for the cap test.
  [string]$Env = "esp32-can-x2-captest",
  # Unattended launch: skip the "press Enter" at the end. With nobody at the
  # keyboard that prompt waits forever and the window never closes, which looks
  # exactly like a hung run. Everything is already on disk by then.
  [switch]$NoPause
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$stamp = Get-Date -Format 'yyyy-MM-dd-HHmm'
# Rule 3: a fixed, dated folder in the repo's analysis dir. Never %TEMP%,
# which Windows may clear before anyone reads it.
$out  = Join-Path $repo "analysis\bench-$stamp"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$pio = "C:\Users\turru\.platformio\penv\Scripts\pio.exe"
# `pio pkg exec -p tool-esptoolpy -- esptool` fails with WinError 2 on this
# install -- the package ships esptool.py but no matching console entry point.
# Call the script through PlatformIO's own interpreter instead.
$pioPy   = "C:\Users\turru\.platformio\penv\Scripts\python.exe"
$esptool = "C:\Users\turru\.platformio\packages\tool-esptoolpy\esptool.py"
# Known trap: without this, pio upload dies after ~10 min on a cp1252
# UnicodeEncodeError that names nothing relevant. Same for our own tools,
# whose help text and logs contain non-cp1252 characters.
$env:PYTHONIOENCODING = "utf-8"

function Log($m) {
  $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
  Write-Host $line
  Add-Content -Path (Join-Path $out "runner.log") -Value $line -Encoding utf8
}

Log "=== cardiag clean-partition bench run ==="
Log "output -> $out"

# --- 0. PRE-FLIGHT (rule 5) -------------------------------------------------
Log "--- pre-flight ---"

$scheme = (powercfg /getactivescheme) -replace '.*GUID: ([a-f0-9-]+).*','$1'
$sleepAC = ((powercfg /q $scheme SUB_SLEEP STANDBYIDLE |
             Select-String 'Current AC Power Setting Index') -replace '.*:\s*','').Trim()
if ($sleepAC -ne '0x00000000') {
  Log "!! sleep on AC is $sleepAC, not Never. Setting it."
  powercfg /change standby-timeout-ac 0
}
Log "sleep on AC: Never"

# --- CONFIG GUARD CHECK -----------------------------------------------------
# A -D flag that a header quietly redefines does nothing, and the run still
# produces numbers that get believed. That has now cost two bench campaigns
# (FS_SNAPSHOT_PERIOD_MS, then FS_TIER_A_MAX_PCT). Refuse to start rather than
# spend 20 minutes measuring a cap the board does not have.
Log "--- config guard check ---"
& python (Join-Path $PSScriptRoot 'check_config_guards.py') 2>&1 |
  Tee-Object -FilePath (Join-Path $out "guards.log") -Append |
  ForEach-Object { Log "  $_" }
if ($LASTEXITCODE -ne 0) {
  Log "!! unguarded overridable macro(s) -- a build flag would be SILENTLY"
  Log "   IGNORED and this run would measure the wrong thing. ABORTING."
  exit 7
}

$hs = & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hotspot.ps1') -Action state
if ($hs -notmatch 'On') {
  Log "hotspot off -> starting"
  & powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'hotspot.ps1') -Action start | Out-Null
  Start-Sleep -Seconds 3
}
Log "hotspot: on"

if (-not (Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
          Where-Object DeviceID -eq $Port)) {
  # Win32_SerialPort misses USB CDC devices on some systems; fall back to the
  # .NET enumeration rather than aborting a run over a flaky WMI class.
  if (-not ([System.IO.Ports.SerialPort]::GetPortNames() -contains $Port)) {
    Log "!! $Port not found. Is the board plugged in? ABORTING."
    exit 2
  }
}
Log "$Port present"

# --- 1. BASELINE: read the loss record BEFORE erasing anything --------------
# Claim 5 needs the pre-erase number, and it can only be read while the old
# firmware is still up and on the network. If the board is not reachable we
# carry on without claim 5 rather than abandoning the run.
$baseline = $null
$oldIp = $null
try {
  $ipLine = Select-String -Path (Join-Path $repo "analysis\*\serial.log") -Pattern 'STA up: ip=([0-9.]+)' -ErrorAction SilentlyContinue |
            Select-Object -Last 1
  if ($ipLine) { $oldIp = [regex]::Match($ipLine.Line, 'ip=([0-9.]+)').Groups[1].Value }
} catch {}
foreach ($cand in @($oldIp, '192.168.137.162') | Where-Object { $_ }) {
  try {
    $s = Invoke-RestMethod -Uri "http://$cand/api/v1/session" -TimeoutSec 5
    $baseline = [int]$s.storage.lost_files_total
    Log "baseline lifetime loss (pre-erase, from $cand): $baseline"
    break
  } catch { }
}
if ($null -eq $baseline) { Log "board not reachable pre-erase; claim 5 will be skipped" }

# --- 2. ERASE THE SPIFFS PARTITION ------------------------------------------
# 0x410000 + 0x3E0000, from firmware/partitions_cardiag_8mb.csv. NVS lives at
# 0x9000 and is NOT touched: the lifetime loss counters must survive this, and
# claim 5 checks exactly that.
Log "--- erasing spiffs partition (0x410000, 0x3E0000) ---"
Push-Location (Join-Path $repo 'firmware')
& $pioPy $esptool --chip esp32s3 --port $Port erase-region 0x410000 0x3E0000 2>&1 |
  Tee-Object -FilePath (Join-Path $out "erase.log") -Append | Out-Null
if ($LASTEXITCODE -ne 0) { Pop-Location; Log "!! erase FAILED (see erase.log). ABORTING."; exit 3 }
Log "spiffs erased -- LittleFS will reformat on next mount"

# --- 3. FLASH THE FSTEST BUILD ----------------------------------------------
# fstest feeds SELFTEST frames into the change log, which is the only way to
# grow Tier A on a desk with no car attached.
Log "--- flashing $Env ---"
& $pio run -e $Env -t upload --upload-port $Port 2>&1 |
  Tee-Object -FilePath (Join-Path $out "flash.log") -Append | Out-Null
$flashRc = $LASTEXITCODE
Pop-Location
if ($flashRc -ne 0) { Log "!! flash FAILED (see flash.log). ABORTING."; exit 4 }
Log "flashed"

# --- 4. SERIAL CAPTURE FOR THE WHOLE WINDOW ---------------------------------
# One process owns COM3 and does both jobs: it captures continuously (so the
# boot banner and any reset reason are recorded -- the reboot question) AND
# sends the mode keys at the right moment. Two processes cannot share the port.
#   3 = SELFTEST (transmits), y = confirm, l = start the change log
$serialSecs = ($Minutes * 60) + 180
Log "--- starting serial capture ($serialSecs s) + mode keys ---"
$serialArgs = @(
  (Join-Path $PSScriptRoot 'serial_capture.py'),
  '--port', $Port, '--seconds', $serialSecs,
  '--delay', '25', '--gap', '3',
  '--send', '3', '--send', 'y', '--send', 'l',
  '--out', (Join-Path $out 'serial.log'), '--quiet'
)
$serial = Start-Process python -ArgumentList $serialArgs -PassThru -NoNewWindow

# --- 5. DISCOVER THE BOARD'S IP FROM THE LIVE SERIAL LOG --------------------
# serial_capture flushes every line, so this can read the log while it is still
# being written. Bench IP has been six different addresses -- never hardcode it.
Log "waiting for the board to join the hotspot..."
$ip = $null
for ($i = 0; $i -lt 90; $i++) {
  Start-Sleep -Seconds 2
  $m = Select-String -Path (Join-Path $out 'serial.log') -Pattern 'STA up: ip=([0-9.]+)' -ErrorAction SilentlyContinue |
       Select-Object -Last 1
  if ($m) { $ip = [regex]::Match($m.Line, 'ip=([0-9.]+)').Groups[1].Value; break }
}
if (-not $ip) {
  Log "!! board never reported an IP. Hotspot up? See serial.log. ABORTING."
  if (-not $serial.HasExited) { Stop-Process -Id $serial.Id -Force }
  exit 5
}
Log "board at $ip"

# --- 5b. ASSERT THE BOARD IS RUNNING THE FLAGS WE ASKED FOR -----------------
# The guard check above stops the KNOWN cause. This checks the EFFECT, on the
# board, and so catches causes nobody has thought of yet: a misspelled flag, a
# stale binary that never reflashed, an `extends` that does not inherit what it
# appears to, a value clamped at runtime.
#
# The board printed its effective caps at mount and serial.log already has the
# line by now -- IP discovery above read past it. Abort, not warn: a run under
# a flag that did not take effect measures something other than what it claims,
# and the 18:40 run proved those numbers get believed.
Log "--- asserting effective caps match $Env ---"
& python (Join-Path $PSScriptRoot 'assert_effective_caps.py') `
    '--env' $Env '--serial' (Join-Path $out 'serial.log') 2>&1 |
  Tee-Object -FilePath (Join-Path $out "caps.log") -Append |
  ForEach-Object { Log "  $_" }
if ($LASTEXITCODE -ne 0) {
  Log "!! effective caps do not match the requested build flags (or could not"
  Log "   be read). See caps.log. ABORTING before wasting the window."
  if (-not $serial.HasExited) { Stop-Process -Id $serial.Id -Force }
  exit 8
}

# --- 6. TOKEN ---------------------------------------------------------------
$secrets = Join-Path $repo 'firmware\include\secrets.h'
$tok = (Select-String -Path $secrets -Pattern '#define HUB_API_TOKEN "([^"]+)"').Matches[0].Groups[1].Value
if (-not $tok) { Log "!! no HUB_API_TOKEN in secrets.h. ABORTING."; exit 6 }
$env:HUB_API_TOKEN = $tok
Log "token loaded (not logged)"

# --- 7. THE WATCH -----------------------------------------------------------
Log "--- retention watch, $Minutes min, ack-after $AckAfter ---"
$watchArgs = @(
  (Join-Path $PSScriptRoot 'retention_watch.py'),
  '--host', $ip,
  '--seconds', ($Minutes * 60),
  '--interval', '3',
  '--ack-after', $AckAfter,
  '--csv', (Join-Path $out 'retention.csv'),
  '--summary', (Join-Path $out 'SUMMARY.md')
)
if ($null -ne $baseline) { $watchArgs += @('--baseline-lost', $baseline) }

& python $watchArgs 2>&1 | Tee-Object -FilePath (Join-Path $out 'retention.log')
$rc = $LASTEXITCODE

# --- 8. WRAP UP -------------------------------------------------------------
if (-not $serial.HasExited) {
  Log "stopping serial capture"
  Stop-Process -Id $serial.Id -Force -ErrorAction SilentlyContinue
}

# The reboot question, answered straight out of the board's own words.
$boots = Select-String -Path (Join-Path $out 'serial.log') -Pattern 'rst:|boot:|Task watchdog|wdt|LIFETIME LOSS' -ErrorAction SilentlyContinue
if ($boots) {
  Log "--- reset/watchdog lines in serial.log (the reboot question) ---"
  $boots | Select-Object -First 20 | ForEach-Object { Log ("  " + $_.Line) }
  Add-Content -Path (Join-Path $out 'SUMMARY.md') -Encoding utf8 -Value @"

## Reset / watchdog lines found in serial.log

``````
$(($boots | Select-Object -First 40 | ForEach-Object { $_.Line }) -join "`n")
``````
"@
} else {
  Log "no reset/watchdog lines in serial.log -- board did not reboot during the run"
  Add-Content -Path (Join-Path $out 'SUMMARY.md') -Encoding utf8 -Value @"

## Reboot question

No reset or watchdog lines appeared in ``serial.log`` for this window, so the
board did NOT reboot during the run. That does not explain the 2026-09-24
reboot; it means this run did not reproduce it.
"@
}

Log ""
Log "=== DONE. verdict rc=$rc ==="
Log "READ: $out\SUMMARY.md"
Log "      $out\retention.log   (full verdicts)"
Log "      $out\retention.csv   (per-sample, flushed live)"
Log "      $out\serial.log      (board output + reset reason)"
Write-Host ""
if (-not $NoPause) {
  Write-Host "Press Enter to close..." -ForegroundColor Cyan
  Read-Host
}
# A marker file is the unattended signal that the run reached the end rather
# than dying partway. Its absence next to a populated folder means the run was
# cut, and the partial CSV/log are still the real data.
Set-Content -Path (Join-Path $out 'DONE.txt') -Encoding utf8 -Value @"
finished $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')
verdict rc=$rc  ($(if ($rc -eq 0) {'PASS'} else {'FAIL or incomplete'}))
read SUMMARY.md first
"@
exit $rc
