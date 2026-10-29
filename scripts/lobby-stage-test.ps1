# Back to the lobby after a match with the choices split, a new stage, then
# both ready: instance 0 hosts, 1 joins, both mash through a match; at the
# decision 0 chooses Character select and 1 Rematch (so the session goes back
# to the lobby over the held results screen), the host picks -NextStage, both
# say ready, and the next match must start on that stage. -NextPick changes
# the host's pick in the lobby too (a costume not loaded yet, say), and
# -StartSeconds is how soon the match must start after both are ready.
#   lobby-stage-test.ps1 -NextPick 'RYU 3 1 0 1 1' -StartSeconds 15
param([string]$Stage = 'CHN', [string]$NextStage = 'NZL', [string]$NextPick = '', [int[]]$Seeds = @(11, 22),
      [int]$StartSeconds = 45)
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
function LogOf($i) { Join-Path $w ($(if ($i -eq 0) { 'SFVR\logs\sfvr.log' } else { "SFVR\logs\sfvr.$i.log" })) }
$n = @{}; foreach ($i in 0, 1) { $n[$i] = @(Get-Content (LogOf $i)).Count }
function Since($i) { Get-Content (LogOf $i) | Select-Object -Skip $n[$i] }
function WaitCount($i, $pattern, $count, $seconds) {
  $end = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $end) { if (@(Since $i | Where-Object { $_ -match $pattern }).Count -ge $count) { return $true }; Start-Sleep -Milliseconds 500 }
  "timed out waiting for $count x '$pattern' on $i"
  $false
}
& (Join-Path $PSScriptRoot 'weyve-server.ps1') | Out-Null
& (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 0 -Commands "server 127.0.0.1:5555; mash $($Seeds[0]); pick RYU 0 1 0 1 1; stage $Stage; host manual" -WaitMs 1500 | Out-Null
$code = (Since 0 | Select-String 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
& (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 1 -Commands "server 127.0.0.1:5555; mash $($Seeds[1]); pick KEN 0 1 0 1 1; join $code manual" -WaitMs 500 | Out-Null
# Manual: ready is said by hand, as players do (auto-ready would start the
# next match from the lobby at once).
if (-not (WaitCount 0 'joined from' 1 30)) { exit 1 }
Start-Sleep 3
foreach ($i in 0, 1) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $i -Commands 'ready' -WaitMs 0 | Out-Null }
$ok = $true
if (-not (WaitCount 0 'match running' 1 120)) { exit 1 }
if (-not (WaitCount 0 'the match is decided' 1 400)) { exit 1 }
Start-Sleep 3
& (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 1 -Commands 'choose rematch' -WaitMs 0 | Out-Null
Start-Sleep 2
& (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 0 -Commands 'choose select' -WaitMs 0 | Out-Null
foreach ($i in 0, 1) { if (-not (WaitCount $i 'back to the lobby' 1 30)) { $ok = $false } }
Start-Sleep 3
& (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 0 -Commands "stage $NextStage$(if ($NextPick) { "; pick $NextPick" })" -WaitMs 0 | Out-Null
Start-Sleep 2
foreach ($i in 0, 1) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $i -Commands 'ready' -WaitMs 0 | Out-Null }
$started = WaitCount 0 'match running' 2 $StartSeconds
if ($started -ne $true) { $ok = $false }
$second = @(Since 0 | Select-String 'netplay: match [0-9a-f]{16};.*stage \d+ (\S+)') | Select-Object -Skip 1 -First 1
if (-not $second -or $second.Matches[0].Groups[1].Value -ne $NextStage) { "the second match: $(if ($second) { $second.Matches[0].Groups[1].Value } else { 'none' })"; $ok = $false }
foreach ($i in 0, 1) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $i -Commands 'leave' -WaitMs 300 | Out-Null }
Start-Sleep 3
$pattern = 'netplay: (match [0-9a-f]{8}|after the match|back to the lobby|both chose|ready|not ready|stage |starting without|jumping into|match running|session over|left)|flow: (preload|stage )|results screen held'
foreach ($i in 0, 1) {
  "== instance $i"
  Since $i | Where-Object { $_ -match $pattern } | ForEach-Object { $_.Substring(0, [Math]::Min(190, $_.Length)) }
}
if ($ok) { "PASS: the next match started on $NextStage" } else { "FAIL" }
