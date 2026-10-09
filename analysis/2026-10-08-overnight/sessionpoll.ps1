# Overnight 10-08: GET /api/v1/session every $Every s for $Minutes min (0 = forever), one line per poll.
param([int]$Every = 30, [int]$Minutes = 10, [string]$Log = 'sessionpoll.log', [string]$Ip = '')
Set-Location C:\Users\turru\Projects\cardiag\analysis\2026-10-08-overnight
$end = if ($Minutes -gt 0) { (Get-Date).AddMinutes($Minutes) } else { [datetime]::MaxValue }
while ((Get-Date) -lt $end) {
  $hosts = @('cardiag.local'); if ($Ip) { $hosts = @($Ip, 'cardiag.local') }
  $line = $null
  foreach ($h in $hosts) {
    try {
      $s = Invoke-RestMethod "http://$h/api/v1/session" -TimeoutSec 10
      $d = $s.heapdiag
      $line = "$(Get-Date -f s) host=$h boot=$($s.boot_id) up=$($s.uptime_ms) " +
        "stack_free_min=$($s.loop_stack.free_min) stack_warn=$($s.loop_stack.warn) " +
        "can=$($s.can_bus.state) tec=$($s.can_bus.tec) rec=$($s.can_bus.rec) recov=$($s.can_bus.recoveries)/$($s.can_bus.last_action) " +
        "checks=$($d.check.checks) fail=$($d.check.failures) mdns_stack_min=$($d.mdns_stack_free_min) parked=$($d.mdns_parked)/$($d.mdns_reaped) " +
        "heap=$($s.heap.free)/$($s.heap.min_free) crashes=$($s.coredump.crashes_total) pkts=$($s.stream.packets) " +
        "hub=$($s.hub_addr.ip)/$($s.hub_addr.source) prev=$($d.prev_boot_fail | ConvertTo-Json -Compress)"
      break
    } catch { $line = "$(Get-Date -f s) UNREACHABLE $h $($_.Exception.Message)" }
  }
  $line | Add-Content -Path $Log -Encoding utf8
  Start-Sleep $Every
}
