# Repeats short netplay matches between instance 0 (host) and 1, each started
# from wherever the games are (the main menu after the previous one) and left
# after $Seconds; prints per match whether the session started, how long the
# start took, and whether both went back to the main menu. A match that does
# not start is reported and the loop goes on with the next.
param([int]$Count = 4, [int]$Seconds = 15, [string]$Server = '127.0.0.1:5555')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib.ps1')
Assert-GamesRunning 2
if ($Server -like '127.0.0.1:*') { Start-RoomsServer ([int]($Server -split ':')[1]) }
Reset-Instances 0, 1 -Server $Server
for ($k = 1; $k -le $Count; $k++) {
  $start = Get-Date
  $tails = @{ 0 = New-GameTail 0; 1 = New-GameTail 1 }
  $started = $true
  try {
    $code = Start-Host 0 "mash $(10 + $k); stage CHN"
    Start-Join 1 $code "mash $(20 + $k)" -Seconds 120 | Out-Null
  }
  catch { $started = $false; "match ${k}: $_" }
  $took = [int]((Get-Date) - $start).TotalSeconds
  if ($started) { Start-Sleep $Seconds }
  Reset-Instances 0, 1
  Start-Sleep 6
  $menu = foreach ($i in 0, 1) { Update-LogTail $tails[$i]; [bool]($tails[$i].Lines -match 'change scene to MainMenuState') }
  $desync = foreach ($i in 0, 1) { $tails[$i].Lines -match 'desync detected' | Select-Object -First 1 }
  "match ${k}: session $(if ($started) { "started after $took s" } else { 'NOT started' }), back at the menu: $($menu -join '/'), desync: $(if ($desync) { 'yes' } else { 'no' })"
  # The menu needs a moment before the next jump.
  Start-Sleep 8
}
