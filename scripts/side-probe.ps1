# Research probe for the score by winner: which of the two
# 372-byte (vitality) regions is P1? Hosts with -P1 on instance 0 and joins
# with -P2 on instance 1; once the fight runs, logs the layout, both regions'
# first word (current / max vitality) and what points at each (findptr).
param([string]$P1 = 'ZGF', [string]$P2 = 'KRN')
$s = $PSScriptRoot
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$log = "$w\SFVR\logs\sfvr.log"
$n0 = @(Get-Content $log).Count
& "$s\cmd.ps1" -Instance 0 -Commands "pick $P1 0 1 0 1 1; host" -WaitMs 2500 | Out-Null
$code = (Get-Content $log | Select-Object -Skip $n0 | Select-String 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
& "$s\cmd.ps1" -Instance 1 -Commands "pick $P2 0 1 0 1 1; join $code" -WaitMs 500 | Out-Null
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
  Start-Sleep 1
  if (Get-Content $log | Select-Object -Skip $n0 | Select-String 'round flow event 6') { break }
}
Start-Sleep 3
$n1 = @(Get-Content $log).Count
& "$s\cmd.ps1" -Instance 0 -Commands 'layout' -WaitMs 1500 | Out-Null
$regions = Get-Content $log | Select-Object -Skip $n1 | Select-String 'region (s372\.\d)\s+(\S+)'
foreach ($r in $regions) {
  $name = $r.Matches.Groups[1].Value; $addr = $r.Matches.Groups[2].Value
  $n2 = @(Get-Content $log).Count
  & "$s\cmd.ps1" -Instance 0 -Commands "peek $addr 1" -WaitMs 600 | Out-Null
  $v = (Get-Content $log | Select-Object -Skip $n2 | Select-String 'peek \S+: (\S+)' | Select-Object -First 1).Matches.Groups[1].Value
  $n3 = @(Get-Content $log).Count
  & "$s\cmd.ps1" -Instance 0 -Commands "findptr $addr" -WaitMs 8000 | Out-Null
  Get-Content $log | Select-Object -Skip $n3 | Select-String 'findptr' | ForEach-Object { "   " + $_.Line.Substring(23) }
  "$name $addr max $([Convert]::ToInt32($v.Substring(0, 4), 16)) now $([Convert]::ToInt32($v.Substring(8, 4), 16))"
}
& "$s\cmd.ps1" -Instance 0 -Commands 'leave' -WaitMs 500 | Out-Null
