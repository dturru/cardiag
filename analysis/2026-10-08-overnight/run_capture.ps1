# Overnight 10-08: esptool reset, then serial capture (default 12 h) with SELFTEST armed at +12 s.
param([int]$Seconds = 43200, [string]$Name = 'serial')
Set-Location C:\Users\turru\Projects\cardiag
$out = 'analysis\2026-10-08-overnight'
$env:PYTHONIOENCODING = 'utf-8'
$py = 'C:\Users\turru\.platformio\penv\Scripts\python.exe'
& $py C:\Users\turru\.platformio\packages\tool-esptoolpy\esptool.py --chip esp32s3 --port COM3 --after hard_reset read_mac *> "$out\reset-$Name.log"
"reset exit=$LASTEXITCODE $(Get-Date -f s)" | Add-Content "$out\reset-$Name.log"
python tools\serial_capture.py --port COM3 --seconds $Seconds --delay 12 --gap 1.2 --send 3 --send y --out "$out\$Name.log" --quiet
"capture $Name ended $(Get-Date -f s) exit=$LASTEXITCODE" | Add-Content "$out\capture-ended.txt"
