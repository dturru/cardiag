<#
.SYNOPSIS
  Start / stop / query the Windows Mobile Hotspot from the command line.

.DESCRIPTION
  The Settings toggle has no CLI, so this drives the WinRT
  NetworkOperatorTetheringManager directly. Used by tools/soak_wifi.py to
  toggle the bench hub AP fifty times without a human clicking anything.

  Windows PowerShell 5.1: WinRT IAsyncOperation is not awaitable natively, so
  the Await helper below reflects out the generic AsTask overload. That pattern
  is load-bearing -- calling .Wait() on the raw IAsyncOperation does not work.

  CreateFromConnectionProfile() needs an ACTIVE internet connection profile: if
  the laptop is offline, this reports NoProfile rather than pretending.

.PARAMETER Action
  state | start | stop | watch

  watch: a watchdog for unattended runs, in its own window. The hotspot
  switches ITSELF off (twice on 2026-10-08: ~16 min and ~1 h 40 min of lost
  polls), and after hours on it can stop passing mDNS multicast between its
  clients while unicast keeps working (10-08 overnight). Every -Every seconds:
  if it is Off, start it; if -Probe names a host and the logger has not
  answered -ProbeFails checks in a row while the hotspot is On, restart it
  (stop, 5 s, start). One log line per action, nothing while healthy.

  The probe is an HTTP GET of http://<Probe><ProbePath>, the same request
  the poller makes, so the name goes through the same resolver (.NET /
  getaddrinfo, where Windows does mDNS). It used to call Resolve-DnsName,
  which reported cardiag.local unresolved while Invoke-RestMethod on the
  same name kept working, and restarted a healthy hotspot. If the name does
  not answer, the probe retries the logger's last known IPv4 address (learned
  from the name on each good pass, or seeded by -ProbeIp); a logger that
  answers by IP counts as reachable. A miss is "neither answers".

  Restarts back off: the gap between restarts in one outage doubles
  (-ProbeFails, 2x, 4x ... up to 60 checks) and resets once the logger
  answers. After -MaxRestarts probe restarts in a run, probe failures are
  only logged; Off -> start keeps working.

.PARAMETER Every
  watch: seconds between checks (default 60).

.PARAMETER Probe
  watch: an mDNS name a client advertises (e.g. cardiag.local). Optional.

.PARAMETER ProbePath
  watch: path the probe GETs (default /api/v1/session, what the poller reads).

.PARAMETER ProbeIp
  watch: IPv4 address to try when -Probe does not answer by name, until one
  is learned from the name. Optional; never required (the bench address
  moves, see CLAUDE.md).

.PARAMETER ProbeFails
  watch: consecutive probe failures before the first restart (default 5).

.PARAMETER MaxRestarts
  watch: probe-triggered restarts allowed in one run (default 6).

.PARAMETER Log
  watch: append actions here as well as to the console.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/hotspot.ps1 -Action state

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/hotspot.ps1 -Action watch -Probe cardiag.local -Log analysis\<run>\hotspot-watch.log
#>
param(
  [ValidateSet('state', 'start', 'stop', 'watch')]
  [string]$Action = 'state',
  [int]$Every = 60,
  [string]$Probe = '',
  [string]$ProbePath = '/api/v1/session',
  [string]$ProbeIp = '',
  [int]$ProbeFails = 5,
  [int]$MaxRestarts = 6,
  [string]$Log = ''
)

$ErrorActionPreference = 'Stop'

# System.WindowsRuntimeSystemExtensions lives in System.Runtime.WindowsRuntime,
# which PowerShell 5.1 does NOT load by default -- without this the reflection
# below fails with a bare "TypeNotFound" that says nothing about the cause.
Add-Type -AssemblyName System.Runtime.WindowsRuntime | Out-Null

$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() |
  Where-Object {
    $_.Name -eq 'AsTask' -and
    $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
  })[0]

function Await($WinRtTask, $ResultType) {
  $asTask = $asTaskGeneric.MakeGenericMethod($ResultType)
  $netTask = $asTask.Invoke($null, @($WinRtTask))
  $netTask.Wait(-1) | Out-Null
  $netTask.Result
}

function Get-Manager {
  $profile = [Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime]::GetInternetConnectionProfile()
  if ($null -eq $profile) { return $null }
  return [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]::CreateFromConnectionProfile($profile)
}

$resultType = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult]

if ($Action -eq 'watch') {
  function Say($msg) {
    $line = "$(Get-Date -Format s) $msg"
    Write-Output $line
    if ($Log) { Add-Content -Path $Log -Value $line -Encoding utf8 }
  }
  # The poller's path, not Resolve-DnsName (see .DESCRIPTION). An HTTP error
  # status still means the logger answered.
  function Answers($target) {
    try {
      Invoke-WebRequest -Uri "http://$target$ProbePath" -UseBasicParsing -TimeoutSec 5 | Out-Null
      return $true
    } catch {
      return ($null -ne $_.Exception.Response)
    }
  }
  function Ipv4Of($name) {
    try {
      $a = [System.Net.Dns]::GetHostAddresses($name) |
        Where-Object AddressFamily -eq 'InterNetwork' | Select-Object -First 1
      if ($a) { return $a.IPAddressToString }
    } catch {}
    return $null
  }
  $gapCap = [Math]::Max($ProbeFails, 60)
  Say "watch: every ${Every}s$(if ($Probe) { ", probe http://$Probe$ProbePath$(if ($ProbeIp) { " (fallback $ProbeIp)" }), restart after $ProbeFails misses, max $MaxRestarts" })"
  $misses = 0; $lastState = ''; $knownIp = $ProbeIp; $via = 'name'
  $gap = $ProbeFails; $nextAt = $ProbeFails; $restarts = 0
  while ($true) {
    try {
      $m = Get-Manager    # every pass: the internet profile changes when Wi-Fi rejoins
      if ($null -eq $m) {
        if ($lastState -ne 'NoProfile') { Say 'no internet connection profile; waiting' }
        $lastState = 'NoProfile'
      } else {
        $st = $m.TetheringOperationalState.ToString()
        if ($st -eq 'Off') {
          $r = Await ($m.StartTetheringAsync()) $resultType
          Say "hotspot was Off -> start: $($r.Status)"
          $misses = 0
        } elseif ($st -eq 'On' -and $Probe) {
          $now = $null
          if (Answers $Probe) {
            $now = 'name'
            $ip = Ipv4Of $Probe
            if ($ip) { $knownIp = $ip }
          } elseif ($knownIp -and (Answers $knownIp)) {
            $now = 'ip'
          }
          if ($now) {
            if ($misses -gt 0) { Say "logger answers again (by $now) after $misses misses" }
            elseif ($now -ne $via) {
              if ($now -eq 'ip') { Say "$Probe does not answer by name, $knownIp does -> no restart (mDNS?)" }
              else { Say "$Probe answers by name again" }
            }
            $via = $now; $misses = 0; $gap = $ProbeFails; $nextAt = $ProbeFails
          } else {
            $misses++
            if ($misses -ge $nextAt) {
              if ($restarts -ge $MaxRestarts) {
                if ($restarts -eq $MaxRestarts) { Say "logger unreachable $misses checks; $MaxRestarts restarts used, not restarting again this run" }
                $restarts++   # one log line, then quiet
              } else {
                $a = Await ($m.StopTetheringAsync()) $resultType
                Start-Sleep 5
                $b = Await ($m.StartTetheringAsync()) $resultType
                $restarts++
                $gap = [Math]::Min($gap * 2, $gapCap)
                Say "logger unreachable $misses checks in a row (name$(if ($knownIp) { " and $knownIp" })) -> restart $restarts/${MaxRestarts}: stop $($a.Status), start $($b.Status); next after $gap more"
              }
              $nextAt = $misses + $gap
            }
          }
        }
        if ($lastState -eq 'NoProfile') { Say "internet profile back (hotspot $st)" }
        $lastState = $st
      }
    } catch {
      Say "error: $($_.Exception.Message)"
    }
    Start-Sleep $Every
  }
}

$mgr = Get-Manager
if ($null -eq $mgr) {
  # Emitted as one token so the caller can branch on it without parsing prose.
  Write-Output 'NoProfile'
  exit 2
}

switch ($Action) {
  'state' {
    Write-Output $mgr.TetheringOperationalState.ToString()
  }
  'start' {
    if ($mgr.TetheringOperationalState -eq 'On') { Write-Output 'AlreadyOn'; break }
    $r = Await ($mgr.StartTetheringAsync()) $resultType
    Write-Output $r.Status.ToString()
  }
  'stop' {
    if ($mgr.TetheringOperationalState -eq 'Off') { Write-Output 'AlreadyOff'; break }
    $r = Await ($mgr.StopTetheringAsync()) $resultType
    Write-Output $r.Status.ToString()
  }
}
