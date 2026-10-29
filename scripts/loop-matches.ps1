# Repeats short netplay matches between instance 0 (host) and 1, each started
# from wherever the games are (the main menu after the previous one) and left
# after $Seconds; prints per match whether the session started, how long the
# start took, and whether both went back to the main menu.
param([int]$Count = 4, [int]$Seconds = 15)
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$logs = @((Join-Path $w 'SFVR\logs\sfvr.log'), (Join-Path $w 'SFVR\logs\sfvr.1.log'))
function Since($log, $mark) { Get-Content $log -Tail 400 | Where-Object { $_.Length -gt 12 -and $_.Substring(0, 12) -ge $mark } }
for ($k = 1; $k -le $Count; $k++) {
  $start = Get-Date
  $mark = $start.ToString('HH:mm:ss.fff')
  & "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands "mash $(10 + $k); stage CHN; host" -WaitMs 800
  $code = (Select-String -Path $logs[0] -Pattern 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
  & "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands "mash $(20 + $k); join $code" -WaitMs 100
  $deadline = $start.AddSeconds(120)
  do { Start-Sleep 1 } until (((Since $logs[1] $mark) -match 'session started as') -or (Get-Date) -gt $deadline)
  $started = [bool]((Since $logs[1] $mark) -match 'session started as')
  $took = [int]((Get-Date) - $start).TotalSeconds
  Start-Sleep $Seconds
  & "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands 'leave' -WaitMs 300
  & "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands 'leave' -WaitMs 6000
  $menu = foreach ($l in $logs) { [bool]((Since $l $mark) -match 'change scene to MainMenuState') }
  $desync = foreach ($l in $logs) { (Since $l $mark) -match 'desync detected' | Select-Object -First 1 }
  "match ${k}: session $(if ($started) { "started after $took s" } else { 'NOT started' }), back at the menu: $($menu -join '/'), desync: $(if ($desync) { 'yes' } else { 'no' })"
  # The menu needs a moment before the next jump.
  Start-Sleep 8
}
