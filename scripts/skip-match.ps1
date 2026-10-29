# Netplay match (instance 0 hosts, 1 joins) where Start is pressed during the
# intro on the chosen sides (none, host, join, both), then both leave. Prints
# per side when the flow events came (seconds from the session start), the
# session frame of FIGHT, whether the skip was logged, and any desync.
#   skip-match.ps1 -Press both
param([ValidateSet('none', 'host', 'join', 'both')][string]$Press = 'both', [int]$PressAfterMs = 1500,
      [int]$Seconds = 25, [string]$Stage = 'CHN')
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$logs = @((Join-Path $w 'sfv_rollback.log'), (Join-Path $w 'sfv_rollback.1.log'))
function Since($log, $mark) { Get-Content $log -Tail 3000 | Where-Object { $_.Length -gt 12 -and $_.Substring(0, 12) -ge $mark } }
function Stamp($line) { [datetime]::ParseExact($line.Substring(0, 12), 'HH:mm:ss.fff', $null) }
$mark = (Get-Date).ToString('HH:mm:ss.fff')
& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands "mash 0; stage $Stage; host" -WaitMs 800 | Out-Null
$code = (Select-String -Path $logs[0] -Pattern 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
& "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands "mash 0; join $code" -WaitMs 100 | Out-Null
$deadline = (Get-Date).AddSeconds(120)
do { Start-Sleep -Milliseconds 200 } until ((((Since $logs[0] $mark) -match 'session started as') -and ((Since $logs[1] $mark) -match 'session started as')) -or (Get-Date) -gt $deadline)
Start-Sleep -Milliseconds $PressAfterMs
$presses = (1..8 | ForEach-Object { 'pad 0 start 80 250' }) -join ';'
if ($Press -in 'host', 'both') { & "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands $presses -WaitMs 0 | Out-Null }
if ($Press -in 'join', 'both') { & "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands $presses -WaitMs 0 | Out-Null }
Start-Sleep $Seconds
& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands 'leave' -WaitMs 300 | Out-Null
& "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands 'leave' -WaitMs 3000 | Out-Null
foreach ($i in 0, 1) {
  $lines = Since $logs[$i] $mark
  $t0 = $lines | Where-Object { $_ -match 'session started as' } | Select-Object -First 1
  if (-not $t0) { "instance ${i}: no session"; continue }
  $ev = foreach ($e in ($lines | Where-Object { $_ -match 'round flow event (\d+)$' })) {
    if ((Stamp $e) -ge (Stamp $t0) -and $e -match 'event (\d+)$') { '{0}@{1:N1}' -f $Matches[1], ((Stamp $e) - (Stamp $t0)).TotalSeconds }
  }
  $fight = ($lines | Select-String 'prediction window \d+ from frame (\d+) \(fight\)' | Select-Object -First 1).Matches.Groups[1].Value
  $skip = [bool]($lines -match 'both players pressed Start')
  $desync = ($lines -match 'desync detected' | Select-Object -First 1)
  "instance ${i}: $($ev -join ' ') | fight from frame $fight | skip logged: $skip | desync: $(if ($desync) { $desync.Substring(21) } else { 'no' })"
}
