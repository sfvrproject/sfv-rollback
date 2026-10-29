# Looks for frame-0 desyncs per character (stale pool data).
# For each playable character: instance 0 first plays a local battle with it
# (so the two games' histories differ, as two players' would), then the two
# instances play a short netplay match, the character (host) vs $Opponent,
# both mashing. Prints per character whether the frozen starting states
# matched, and the recordings to compare (`sfvr peers`, `sfvr changes`).
param([string]$Opponent = 'KEN', [int]$Seconds = 10, [string[]]$Only = @(), [string]$Out = '')
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$logs = @((Join-Path $w 'sfv_rollback.log'), (Join-Path $w 'sfv_rollback.1.log'))
$sfvr = Join-Path $PSScriptRoot '..\build\Release\sfvr.exe'
function Since($log, $mark) { Get-Content $log -Tail 600 | Where-Object { $_.Length -gt 12 -and $_.Substring(0, 12) -ge $mark } }
function Mark { (Get-Date).ToString('HH:mm:ss.fff') }
function WaitFor($log, $mark, $pattern, $seconds) {
  $deadline = (Get-Date).AddSeconds($seconds)
  do {
    Start-Sleep 1
    $hit = Since $log $mark | Select-String $pattern | Select-Object -First 1
  } until ($hit -or (Get-Date) -gt $deadline)
  return $hit
}
function Say($text) { $text; if ($Out) { Add-Content -Path $Out -Value $text } }

& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands 'overlay hide' -WaitMs 100
& "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands 'overlay hide' -WaitMs 100

# The playable roster from the game's table: CODE=NAME(costumes).
$m = Mark
& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands 'charas' -WaitMs 1500
$line = (Since $logs[0] $m | Select-String 'characters \(\d+\):' | Select-Object -Last 1).Line
$roster = [regex]::Matches($line, '(\w{3})=([^()]*)\((\d+)\)') | Where-Object {
  # X codes are helpers (X01: the partner Akira's V-Trigger calls in), not fighters.
  [int]$_.Groups[3].Value -ge 10 -and $_.Groups[2].Value -and -not $_.Groups[2].Value.StartsWith('*') -and
  -not $_.Groups[2].Value.StartsWith('ID_') -and -not $_.Groups[1].Value.StartsWith('X')
} | ForEach-Object { [pscustomobject]@{ Code = $_.Groups[1].Value; Name = $_.Groups[2].Value } }
if ($Only.Count) { $roster = $roster | Where-Object { $Only -contains $_.Code } }
Say "scanning $(@($roster).Count) characters vs $Opponent"

foreach ($c in $roster) {
  # 1. A local battle with the character on instance 0 only.
  $m = Mark
  & "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands "setup $($c.Code) 0 1 0 1 1 $Opponent 0 1 0 1 1 0; battlenow" -WaitMs 100
  $local = WaitFor $logs[0] $m 'round flow event 3' 90
  # 2. The netplay match.
  $m = Mark
  & "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands "mash 11; pick $($c.Code) 0 1 0 1 1; stage CHN; host" -WaitMs 800
  $code = (Since $logs[0] $m | Select-String 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
  & "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands "mash 22; pick $Opponent 0 1 0 1 1; join $code" -WaitMs 100
  $frozen = WaitFor $logs[0] $m 'frozen state checksums' 120
  $started = WaitFor $logs[1] $m 'session started as' 30
  if ($started) { Start-Sleep $Seconds }
  & "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands 'leave' -WaitMs 300
  & "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands 'leave' -WaitMs 4000
  $files = foreach ($l in $logs) { (Since $l $m | Select-String 'recording (\S+\.sfvr)' | Select-Object -Last 1).Matches.Groups[1].Value }
  $state = if (-not $local) { 'no local battle' } elseif (-not $frozen) { 'no match' } elseif ($frozen.Line -match '\(differ\)') { 'DIFFER' } else { 'same' }
  # The pool bookkeeping regions (600, 1408) are masked on the grounds that a
  # match never writes them: check that here too.
  $pools = 'pools untouched'
  if ($files[0] -and (Test-Path $files[0])) {
    $changed = foreach ($size in 600, 1408) { & $sfvr changes $files[0] $size | Where-Object { $_ -notmatch 'changed on 0 of' } }
    if ($changed) { $pools = 'POOLS WRITTEN: ' + (($changed | ForEach-Object { ($_ -split ' ')[0] }) -join ',') }
  }
  Say ("{0} {1,-16} {2,-7} {3,-16} {4}" -f $c.Code, $c.Name, $state, $pools, ($files -join ' '))
  Start-Sleep 6 # back at the main menu
}
Say 'done'
