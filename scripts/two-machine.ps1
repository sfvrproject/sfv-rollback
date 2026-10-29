# A netplay session between this PC and a second one running scripts\agent.ps1.
# This PC runs the Weyvelength server and game instance 0 (the host); the
# second PC's instance 0 joins. Both mash; after each decided match both make
# the next choice from -Choices; then both leave. The second PC's recordings
# are fetched and each match is compared with `sfvr peers`.
#   two-machine.ps1 -Agent 192.168.1.20 -Token <secret> -Server 192.168.1.10 [-Deploy] [-Choices rematch,select]
# -Server: this PC's address as the second PC reaches it (the server's port 5555
# must be allowed in this PC's firewall). -Deploy first copies this build's mod
# DLL and noise file to the second PC and restarts its game. -Delay: both
# players' input delay (default 0: the most rollbacks). -Choices with no
# entries ('-Choices @()') plays one match.
param([Parameter(Mandatory)][string]$Agent, [Parameter(Mandatory)][string]$Token, [Parameter(Mandatory)][string]$Server,
      [string[]]$Choices = @('rematch', 'select'), [string[]]$Picks = @('RYU', 'KEN'), [string]$Stage = 'CHN',
      [switch]$Deploy, [switch]$NoMash, [int]$EndHoldSeconds = 0, [int]$TimeoutSeconds = 1200, [string]$Out = '', [int]$Delay = 0)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'remote.ps1') -Agent $Agent -Token $Token
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$sfvr = Join-Path $PSScriptRoot '..\build\Release\sfvr.exe'
$log0 = Join-Path $w 'SFVR\logs\sfvr.log'
$outDir = if ($Out) { $Out } else { Join-Path $w ("reports\two-machine-" + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
function Say($text) { $text; Add-Content -Path (Join-Path $outDir 'summary.txt') -Value $text }

$serverAddress = if ($Server -match ':') { $Server } else { "${Server}:5555" }
Say "two-machine session: this PC hosts, $Agent joins, server $serverAddress"
$ping = Remote 'ping'
Say "second PC: $($ping.host), $($ping.games) game instance(s) running"

& (Join-Path $PSScriptRoot 'weyve-server.ps1') | ForEach-Object { Say "server: $_" }
# Both games use this server: the second PC through this PC's address.
(Remote 'ini' @{ file = 'sfv_rollback.ini'; section = 'weyve'; key = 'server'; value = $serverAddress }).output | Out-Null
if ($Deploy) {
  # Both: scripts\up.ps1 there copies its build folder's DLL into the game folder.
  Say (RemotePut (Join-Path $PSScriptRoot '..\build\Release\sfv_rollback.dll') 'sfv_rollback.dll')
  Say (RemotePut (Join-Path $PSScriptRoot '..\build\Release\sfv_rollback.dll') 'sfv-rollback\build\Release\sfv_rollback.dll')
  if ($ping.games) { Remote 'stop' | Out-Null; Start-Sleep 3 }
  $ping = @{ games = 0 }
}
if (-not $ping.games) {
  Say 'starting the game on the second PC'
  Say (Remote 'start' @{ count = 1 }).output.Trim()
  Start-Sleep 15 # the menu ignores input for a while after its map loads
}

. (Join-Path $PSScriptRoot 'logtail.ps1')
$tail0 = New-LogTail $log0
function Since { Update-LogTail $tail0; $tail0.Lines.ToArray() }
RemoteLog 0 -FromNow
$remote = New-Object System.Collections.ArrayList

$mash = if ($NoMash) { '' } else { 'mash 11; ' }
& (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 0 -Commands "delay $Delay; ${mash}pick $($Picks[0]) 0 1 0 1 1; stage $Stage; host" -WaitMs 2000 | Out-Null
$code = (Since | Select-String 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
if (-not $code) { Say 'no code: hosting failed (see the log)'; exit 1 }
Say "code $code"
$mash = if ($NoMash) { '' } else { 'mash 22; ' }
# Its instance 0 is the session's second player.
Remote 'cmd' @{ instance = 0; commands = "name Player 2; delay $Delay; ${mash}pick $($Picks[1]) 0 1 0 1 1; join $code" } | Out-Null

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$decided = 0
while ((Get-Date) -lt $deadline) {
  Start-Sleep 1
  foreach ($l in RemoteLog 0) { [void]$remote.Add($l) }
  $here = Since
  if ($here -match 'session over' -or $remote -match 'session over') { Say 'the session ended early'; break }
  $now = @($here | Where-Object { $_ -match 'the match is decided' }).Count
  if ($now -le $decided) { continue }
  $decided = $now
  Say "match $decided decided"
  Start-Sleep 2
  if ($decided -gt $Choices.Count) {
    # -EndHoldSeconds: stay on the last match's post-match screen a while
    # (to look at it) before both leave.
    if ($EndHoldSeconds -gt 0) { Say "holding the post-match screen for $EndHoldSeconds s"; Start-Sleep $EndHoldSeconds }
    break
  }
  $c = "choose $($Choices[$decided - 1])"
  & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 0 -Commands $c -WaitMs 0 | Out-Null
  Remote 'cmd' @{ instance = 0; commands = $c } | Out-Null
}
Start-Sleep 2
# Mashing and simulated network conditions stay set in the game until a reload: a
# session played by hand afterwards would get them. Off again.
& (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 0 -Commands 'leave; mash 0; netsim 0 0 0' -WaitMs 300 | Out-Null
Remote 'cmd' @{ instance = 0; commands = 'leave; mash 0; netsim 0 0 0' } | Out-Null
Start-Sleep 4
foreach ($l in RemoteLog 0) { [void]$remote.Add($l) }
$here = Since
$here | Set-Content (Join-Path $outDir 'local.log')
$remote | Set-Content (Join-Path $outDir 'remote.log')

$pattern = 'netplay: (match [0-9a-f]{8}:|session started as|the match is decided|both chose|back to the lobby|session over|desync detected|frozen state checksums|link|peer connected)|desync detected|flow: results screen held'
Say '== this PC'
$here | Where-Object { $_ -match $pattern } | ForEach-Object { Say $_.Substring(0, [Math]::Min(170, $_.Length)) }
Say '== second PC'
$remote | Where-Object { $_ -match $pattern } | ForEach-Object { Say $_.Substring(0, [Math]::Min(170, $_.Length)) }

# Each match's two recordings, side by side.
# Keyed by the match id in the file name: the side that leaves logs its last
# match as "left at frame N; recording ...". Paths may hold spaces (Program Files).
$mine = @{}
foreach ($l in $here) { if ($l -match 'recording (.+-([0-9a-f]{8})-p\d-match\.sfvr)') { $mine[$Matches[2]] = $Matches[1] } }
foreach ($l in $remote) {
  if ($l -notmatch 'recording (.+-([0-9a-f]{8})-p\d-match\.sfvr)') { continue }
  $id = $Matches[2]
  $rel = $Matches[1] -replace '^.*\\Binaries\\Win64\\', ''
  $local = Join-Path $outDir (Split-Path $rel -Leaf)
  RemoteGet $rel $local | Out-Null
  if ($mine.ContainsKey($id)) {
    Say "== match ${id}: sfvr peers"
    & $sfvr peers $mine[$id] $local 2>&1 | Select-Object -First 12 | ForEach-Object { Say "  $_" }
  }
}
Say "files in $outDir"
