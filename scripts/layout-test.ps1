# Button layouts across two PCs: each side gets its own custom Button Settings (pad 0's
# live table, `layout`) and keyboard keys (Input2.ini, `input2`), then in a netplay match
# each presses every pad button and its remapped keys; both recordings must show each
# player's presses as that player's own layout makes them. This PC hosts (P1); the
# second PC (scripts\agent.ps1) joins (P2). Input2.ini is restored on both at the end.
#   layout-test.ps1 -Agent 192.168.1.20 -Token <secret> -Server 192.168.1.10 [-Deploy]
param([Parameter(Mandatory)][string]$Agent, [Parameter(Mandatory)][string]$Token, [Parameter(Mandatory)][string]$Server,
      [switch]$Deploy)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'remote.ps1') -Agent $Agent -Token $Token
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$sfvr = Join-Path $PSScriptRoot '..\build\Release\sfvr.exe'
$log0 = Join-Path $w 'SFVR\logs\sfvr.log'
$serverAddress = if ($Server -match ':') { $Server } else { "${Server}:5555" }
function Local($c, $ms = 500) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 0 -Commands $c -WaitMs $ms | Out-Null }
function RemoteCmd($c) { Remote 'cmd' @{ instance = 0; commands = $c } | Out-Null }
function LocalSince($n) { Get-Content $log0 -Encoding utf8 | Select-Object -Skip $n }

# The layouts. Pad tables: up, down, left, right, X, Y, A, B, LB, RB, LT, RT, L3, R3, start, select.
$table = @{
  local  = '1 2 4 8 800 80 200 10 8c0 20 230 40 0 0 4000 8000'
  remote = '1 2 4 8 40 10 20 800 200 8c0 80 230 0 0 4000 8000'
}
# KeyboardKeys_4..9 feed pad A, B, X, Y, LB, RB.
$keys = @{ local = @('U', 'I', 'O', 'P', 'L', 'Y'); remote = @('Z', 'X', 'C', 'V', 'F', 'R') }
$padOrder = @('x', 'y', 'a', 'b', 'lb', 'rb', 'lt', 'rt')
function Expected($side) {
  $t = $table[$side].Split(' ') | ForEach-Object { [Convert]::ToUInt32($_, 16) }
  $pads = @($t[4], $t[5], $t[6], $t[7], $t[8], $t[9], $t[10], $t[11])          # x y a b lb rb lt rt
  $viaKeys = @($t[6], $t[7], $t[4], $t[5], $t[8], $t[9])                        # A B X Y LB RB
  return @($pads + $viaKeys)
}

"== setting up the keyboard layouts (from the next start)"
if ($Deploy) {
  # Both: its up.ps1 copies its build folder's DLL into the game folder.
  $dll = Join-Path $PSScriptRoot '..\build\Release\sfv_rollback.dll'
  RemotePut $dll 'sfv_rollback.dll' | Out-Null
  RemotePut $dll 'sfv-rollback\build\Release\sfv_rollback.dll' | Out-Null
}
& (Join-Path $PSScriptRoot 'up.ps1') -Count 1 -Restart | Out-Null
$i = 4; foreach ($k in $keys.local) { Local "input2 KeyboardKeys_$i $k" 200; $i++ }
if ((Remote 'ping').games -eq 0) { Remote 'start' @{ count = 1 } | Out-Null }
$i = 4; foreach ($k in $keys.remote) { RemoteCmd "input2 KeyboardKeys_$i $k"; $i++ }
Start-Sleep 2
"== restarting both games with them"
Remote 'stop' | Out-Null
Start-Sleep 3
Remote 'start' @{ count = 1 } | Out-Null
& (Join-Path $PSScriptRoot 'up.ps1') -Count 1 -Restart | Out-Null

"== the match"
& (Join-Path $PSScriptRoot 'weyve-server.ps1') | Out-Null
$n = @(Get-Content $log0).Count
Local "server 127.0.0.1:$($serverAddress.Split(':')[1]); delay 2; mash 0; pick RYU 0 1 0 1 1; stage CHN; host" 2000
$code = LocalSince $n | Select-String 'hosting, code (\S+)' | Select-Object -Last 1 | ForEach-Object { $_.Matches[0].Groups[1].Value }
if (-not $code) { 'no code: hosting failed'; exit 1 }
RemoteCmd "server $serverAddress; name Player 2; delay 2; mash 0; pick KEN 0 1 0 1 1; join $code"
$end = (Get-Date).AddSeconds(150)
while ((Get-Date) -lt $end -and -not (LocalSince $n | Select-String -Quiet 'round flow event 6')) { Start-Sleep 1 }
Start-Sleep 2
Local "layout $($table.local)" 500
RemoteCmd "layout $($table.remote)"
Start-Sleep 1
"P1 presses (this PC)"
foreach ($b in $padOrder) { Local "pad 0 $b 200 300" 700 }
& (Join-Path $PSScriptRoot 'win.ps1') -Action keys -Instance 0 -Keys ($keys.local -join ' ') -HoldMs 200 -GapMs 400 | Out-Null
Start-Sleep 1
"P2 presses (second PC)"
foreach ($b in $padOrder) { RemoteCmd "pad 0 $b 200 300"; Start-Sleep -Milliseconds 700 }
RemoteCmd "keys $($keys.remote -join ' ') 200"
Start-Sleep 6
Local 'leave' 500
RemoteCmd 'leave'
Start-Sleep 4

"== the recordings"
$matchId = (LocalSince $n | Select-String 'netplay: match ([0-9a-f]{8})[0-9a-f]{8};' | Select-Object -Last 1).Matches.Groups[1].Value
if (-not $matchId) { 'no match started (did the second PC join?)'; Local 'input2 restore' 500; RemoteCmd 'input2 restore'; exit 1 }
$mine = Get-ChildItem (Join-Path $w 'SFVR\matches') -Filter "*_${matchId}_p1.sfvr" | Select-Object -Last 1
$out = Join-Path $env:TEMP "layout-test-$matchId"
New-Item -ItemType Directory -Force $out | Out-Null
# Its name as the second PC saved it (its clock can be a second apart).
$remoteName = (Remote 'log' @{ from = 0 }).lines | Select-String "netplay: saved .*\\(\S+_${matchId}_p2\.sfvr)" |
  Select-Object -Last 1 | ForEach-Object { $_.Matches.Groups[1].Value }
if (-not $remoteName) { $remoteName = $mine.Name -replace '_p1\.sfvr$', '_p2.sfvr' }
$theirs = Join-Path $out $remoteName
try { RemoteGet "SFVR\matches\$remoteName" $theirs | Out-Null } catch { "cannot fetch $remoteName from the second PC: $_" }
# Each player's distinct non-zero button values, in the order they came (directions left out).
function Presses($file, $player) {
  $raw = & $sfvr inputs $file 0 99999 --raw 2>$null
  if (-not $raw) { $raw = & $sfvr inputs $file 0 99999 }
  return $raw
}
$ok = $true
foreach ($file in @($mine.FullName, $theirs)) {
  if (-not (Test-Path $file)) { $ok = $false; continue }
  foreach ($p in 0, 1) {
    $side = if ($p -eq 0) { 'local' } else { 'remote' }
    $got = & $sfvr presses $file $p 2>&1
    $want = (Expected $side | ForEach-Object { '{0:x}' -f $_ }) -join ' '
    $gotText = ($got -join ' ').Trim()
    $match = $gotText -eq $want
    if (-not $match) { $ok = $false }
    "{0} P{1}: got  {2}`n{3}     want {4} {5}" -f (Split-Path $file -Leaf), ($p + 1), $gotText, (' ' * (Split-Path $file -Leaf).Length), $want, $(if ($match) { 'OK' } else { 'DIFFERENT' })
  }
}
LocalSince $n | Select-String 'desync|left at frame' | ForEach-Object { $_.Line.Substring(13, [Math]::Min(150, $_.Line.Length - 13)) }

"== restoring Input2.ini on both (from the next start)"
Local 'input2 restore' 500
RemoteCmd 'input2 restore'
Start-Sleep 2
Remote 'stop' | Out-Null
if ($ok) { 'PASS: both recordings show each player''s presses as their own layout makes them' } else { 'FAIL' }
