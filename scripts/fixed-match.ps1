# One fixed test match: reload the mod, Ryu (host, sfv 0) vs Ken
# (sfv 1), both mashing with fixed seeds, for $Seconds; then leave and print
# where the recordings first differ.
param([int]$Seconds = 55, [switch]$NoReload, [string]$HostPick = 'RYU 0 1 0 1 1', [string]$JoinPick = 'KEN 0 1 0 1 1',
      [int]$HostMash = 11, [int]$JoinMash = 22, [string]$Stage = 'CHN', [int]$MashFrom = 0)
$t = $env:TEMP
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$S = (Join-Path $PSScriptRoot '..\build\Release\sfvr.exe')
if (-not $NoReload) { & "$PSScriptRoot\reload.ps1" -Instances 0,1 | Out-Null; Start-Sleep 3 }
$mark = (Get-Date).ToString('HH:mm:ss.fff')
& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands "mash $HostMash $MashFrom; pick $HostPick; stage $Stage; host" -WaitMs 800
$code = (Select-String -Path "$w\sfv_rollback.log" -Pattern 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
& "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands "mash $JoinMash $MashFrom; pick $JoinPick; join $code" -WaitMs 100
$deadline = (Get-Date).AddSeconds(120)
do { Start-Sleep 1; $l = Get-Content "$w\sfv_rollback.1.log" | Where-Object { $_.Length -gt 12 -and $_.Substring(0,12) -ge $mark } } until (($l | Select-String 'session started as') -or (Get-Date) -gt $deadline)
Start-Sleep $Seconds
& "$PSScriptRoot\win.ps1" -Action shot -Instance 0 -Out "$t\m0.png" | Out-Null
& "$PSScriptRoot\win.ps1" -Action shot -Instance 1 -Out "$t\m1.png" | Out-Null
& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands 'leave' -WaitMs 300
& "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands 'leave' -WaitMs 2500
$files = foreach ($f in 'sfv_rollback.log', 'sfv_rollback.1.log') { (Select-String -Path "$w\$f" -Pattern 'recording (\S+\.sfvr)' | Select-Object -Last 1).Matches.Groups[1].Value }
Select-String -Path "$w\sfv_rollback.log" -Pattern 'frozen state' | Select-Object -Last 1 | ForEach-Object { $_.Line }
& $S info $files[0] | Select-String 'frames  |desync at' | Select-Object -First 4 | ForEach-Object { $_.Line }
& $S peers $files[0] $files[1] | Select-String '^  frame' | Select-Object -First 6 | ForEach-Object { $_.Line }
& $S peers $files[0] $files[1] --first-change | Select-String 'first changes|size     16' | ForEach-Object { $_.Line }
"files: $($files -join ' ')"
