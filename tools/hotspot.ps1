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
  if it is Off, start it; if -Probe names a host and that name has failed to
  resolve -ProbeFails checks in a row while the hotspot is On, restart it
  (stop, 5 s, start). One log line per action, nothing while healthy.

.PARAMETER Every
  watch: seconds between checks (default 60).

.PARAMETER Probe
  watch: an mDNS name a client advertises (e.g. cardiag.local). Optional.

.PARAMETER ProbeFails
  watch: consecutive probe failures before a restart (default 5).

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
  [int]$ProbeFails = 5,
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
  function Resolves($name) {
    try {
      $r = Resolve-DnsName $name -Type A -ErrorAction Stop | Where-Object Type -eq 'A'
      return [bool]$r
    } catch { return $false }
  }
  Say "watch: every ${Every}s$(if ($Probe) { ", probe $Probe, restart after $ProbeFails misses" })"
  $misses = 0; $lastState = ''
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
          if (Resolves $Probe) {
            if ($misses -ge $ProbeFails) { Say "$Probe resolves again" }
            $misses = 0
          } else {
            $misses++
            if ($misses % $ProbeFails -eq 0) {   # again every $ProbeFails misses, not every pass
              $a = Await ($m.StopTetheringAsync()) $resultType
              Start-Sleep 5
              $b = Await ($m.StartTetheringAsync()) $resultType
              Say "$Probe unresolved $misses checks in a row -> restart: stop $($a.Status), start $($b.Status)"
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
