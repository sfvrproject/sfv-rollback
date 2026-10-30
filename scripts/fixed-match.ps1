# One fixed test match: reload the mod, Ryu (host, sfv 0) vs Ken
# (sfv 1), both mashing with fixed seeds, for $Seconds; then leave and print
# where the recordings first differ.
param([int]$Seconds = 55, [switch]$NoReload, [string]$HostPick = 'RYU 0 1 0 1 1', [string]$JoinPick = 'KEN 0 1 0 1 1',
      [int]$HostMash = 11, [int]$JoinMash = 22, [string]$Stage = 'CHN', [int]$MashFrom = 0, [string]$Server = '127.0.0.1:5555')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
$t = $env:TEMP
$S = (Join-Path $PSScriptRoot '..\build\Release\sfvr.exe')
Assert-GamesRunning 2
if (-not $NoReload) { & "$PSScriptRoot\reload.ps1" -Instances 0,1 | Out-Null; Start-Sleep 3 }
if ($Server -like '127.0.0.1:*') { Start-RoomsServer ([int]($Server -split ':')[1]) }
Reset-Instances 0, 1 -Server $Server
$tails = @{ 0 = New-GameTail 0; 1 = New-GameTail 1 }
$code = Start-Host 0 "mash $HostMash $MashFrom; pick $HostPick; stage $Stage"
Start-Join 1 $code "mash $JoinMash $MashFrom; pick $JoinPick" | Out-Null
Start-Sleep $Seconds
& "$PSScriptRoot\win.ps1" -Action shot -Instance 0 -Out "$t\m0.png" | Out-Null
& "$PSScriptRoot\win.ps1" -Action shot -Instance 1 -Out "$t\m1.png" | Out-Null
Reset-Instances 0, 1
# The recordings this match saved (each side's "recording <file>" line).
$files = foreach ($i in 0, 1) { (Wait-Log $tails[$i] 'recording (\S+\.sfvr)' 20).Groups[1].Value }
Select-String -Path (LogOf 0) -Pattern 'frozen state' | Select-Object -Last 1 | ForEach-Object { $_.Line }
& $S info $files[0] | Select-String 'frames  |desync at' | Select-Object -First 4 | ForEach-Object { $_.Line }
& $S peers $files[0] $files[1] | Select-String '^  frame' | Select-Object -First 6 | ForEach-Object { $_.Line }
& $S peers $files[0] $files[1] --first-change | Select-String 'first changes|size     16' | ForEach-Object { $_.Line }
"files: $($files -join ' ')"
