# Noise mask coverage across loadouts. Every playable character plays one
# netplay match as P1 with V-Skill I / V-Trigger I and one as P2 with V-Skill
# II / V-Trigger II (match k: roster[k] vs roster[k+1]), both sides mashing
# (the mash pattern includes V-Skill and V-Trigger presses). The recordings
# dump the state every 120 frames; `sfvr peers --pointers` learns the words
# that hold addresses on both sides into -Noise (a copy of the noise file is
# made there first), and `sfvr images` the words that point into DLLs.
# Prints per match whether it desynced, and where.
#   scan-loadouts.ps1 -Noise $env:TEMP\noise-scan.txt [-Only RYU,KEN] [-Seconds 75]
param([string]$Noise = (Join-Path $env:TEMP 'sfv_noise.scan.txt'), [int]$Seconds = 75, [string[]]$Only = @(),
      [string]$Out = '', [int]$Start = 0)
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
. (Join-Path $PSScriptRoot 'lib.ps1')
$logs = @((Join-Path $w 'SFVR\logs\sfvr.log'), (Join-Path $w 'SFVR\logs\sfvr.1.log'))
$sfvr = Join-Path $PSScriptRoot '..\build\Release\sfvr.exe'
# Marks are line counts per log (timestamps repeat across days in one log).
function Since($log, $mark) { Get-Content $log | Select-Object -Skip $mark[$log] }
function Mark { $m = @{}; foreach ($l in $logs) { $m[$l] = @(Get-Content $l).Count }; $m }
function WaitFor($log, $mark, $pattern, $seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    Start-Sleep 1
    $hit = Since $log $mark | Select-String $pattern | Select-Object -First 1
  } until ($hit -or (Get-Date) -gt $deadline)
  return $hit
}
function Say($text) { $text; if ($Out) { Add-Content -Path $Out -Value $text } }

if (-not (Test-Path $Noise)) { Copy-Item (Join-Path $PSScriptRoot '..\data\sfv_noise.txt') $Noise }
& "$PSScriptRoot\weyve-server.ps1" | Out-Null
foreach ($i in 0, 1) { & "$PSScriptRoot\cmd.ps1" -Instance $i -Commands 'overlay hide' -WaitMs 100 }

# The playable roster from the game's table: CODE=NAME(costumes).
$m = Mark
& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands 'charas' -WaitMs 1500
$line = (Since $logs[0] $m | Select-String 'characters \(\d+\):' | Select-Object -Last 1).Line
$roster = @([regex]::Matches($line, '(\w{3})=([^()]*)\((\d+)\)') | Where-Object {
  [int]$_.Groups[3].Value -ge 10 -and $_.Groups[2].Value -and -not $_.Groups[2].Value.StartsWith('*') -and
  -not $_.Groups[2].Value.StartsWith('ID_') -and -not $_.Groups[1].Value.StartsWith('X')
} | ForEach-Object { $_.Groups[1].Value })
if ($Only.Count) { $roster = @($roster | Where-Object { $Only -contains $_ }) }
$n = $roster.Count
Say "scanning $n characters, $n matches, noise words into $Noise"

for ($k = $Start; $k -lt $n; $k++) {
  $a = $roster[$k]
  $b = $roster[($k + 1) % $n]
  $before = @(Get-Content $Noise | Where-Object { $_ -match '^\d' }).Count
  $m = Mark
  $code = Start-Host 0 "mash 11; dumps 120 60; pick $a 0 1 0 1 1; stage CHN"
  Start-Join 1 $code "mash 22; dumps 120 60; pick $b 0 1 0 2 2" -NoWait | Out-Null
  $started = WaitFor $logs[1] $m 'session started as' 150
  if ($started) { Start-Sleep $Seconds }
  foreach ($i in 0, 1) { & "$PSScriptRoot\cmd.ps1" -Instance $i -Commands 'leave' -WaitMs 300 | Out-Null }
  Start-Sleep 4
  $files = foreach ($l in $logs) { (Since $l $m | Select-String 'recording (\S+-match\.sfvr)' | Select-Object -Last 1).Matches.Groups[1].Value }
  $desync = foreach ($l in $logs) { Since $l $m | Select-String 'desync detected|checksums differed' | Select-Object -First 1 }
  $state = if (-not $started) { 'NO MATCH' } elseif ($desync) { 'DESYNC ' + (($desync | ForEach-Object { if ($_.Line -match 'frame (\d+)') { $Matches[1] } }) -join '/') } else { 'clean' }
  $learned = ''
  if ($files.Count -eq 2 -and $files[0] -and $files[1] -and (Test-Path $files[0]) -and (Test-Path $files[1])) {
    # Every dump, also after a desync: a word that holds an address on both
    # sides is a pointer field wherever the states went apart.
    & $sfvr peers $files[0] $files[1] --pointers --write $Noise | Out-Null
    # Pointers into DLLs are equal between two processes on one PC (one load
    # address per boot) but not between PCs: learned from either side alone.
    foreach ($f in $files) { & $sfvr images $f all 7FF000000000 FFFFFFFFFF --write $Noise | Out-Null }
    $learned = "+$(@(Get-Content $Noise | Where-Object { $_ -match '^\d' }).Count - $before) words"
  }
  Say ("{0,2} {1} 1/1 vs {2} 2/2  {3,-14} {4,-10} {5}" -f $k, $a, $b, $state, $learned, ($files -join ' '))
  Start-Sleep 6 # back at the main menu
}
Say 'done'
