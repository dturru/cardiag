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
  state | start | stop

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools/hotspot.ps1 -Action state
#>
param(
  [ValidateSet('state', 'start', 'stop')]
  [string]$Action = 'state'
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

$mgr = Get-Manager
if ($null -eq $mgr) {
  # Emitted as one token so the caller can branch on it without parsing prose.
  Write-Output 'NoProfile'
  exit 2
}

$resultType = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult]

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
