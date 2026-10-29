# One netplay session of several matches through the local Weyvelength
# server (scripts/weyve-server.ps1): instance 0 hosts, 1 joins, both mash.
# After each decided match both sides make the next choice from -Choices
# (rematch or select: back to the lobby, where command sessions are ready
# at once); after the last, both leave. -Spectate N adds instance N as a
# spectator. Prints per instance the session's course and any desync.
# -Delay: both players' input delay (default 0: every remote input is
# predicted, the most rollbacks).
#   session.ps1 -Choices rematch,select
param([string[]]$Choices = @('rematch', 'select'), [string]$Stage = 'CHN', [string[]]$Picks = @('RYU', 'KEN'), [int]$Spectate = -1,
      [int]$TimeoutSeconds = 900, [int]$ChooseAfterMs = 1500, [int]$Delay = 0)
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
function LogOf($i) { Join-Path $w ($(if ($i -eq 0) { 'sfv_rollback.log' } else { "sfv_rollback.$i.log" })) }
# The lines each log gained since the start, read as they come (logtail.ps1).
. (Join-Path $PSScriptRoot 'logtail.ps1')
$tails = @{}
function Since($i) { Update-LogTail $tails[$i]; $tails[$i].Lines.ToArray() }
$start = Get-Date
$instances = @(0, 1) + $(if ($Spectate -ge 0) { @($Spectate) } else { @() })
foreach ($i in $instances) { $tails[$i] = New-LogTail (LogOf $i) }
& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands "delay $Delay; mash 11; pick $($Picks[0]) 0 1 0 1 1; stage $Stage; host" -WaitMs 1500 | Out-Null
$code = (Select-String -Path (LogOf 0) -Pattern 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
"code $code"
& "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands "delay $Delay; mash 22; pick $($Picks[1]) 0 1 0 1 1; join $code" -WaitMs 500 | Out-Null
if ($Spectate -ge 0) { & "$PSScriptRoot\cmd.ps1" -Instance $Spectate -Commands "spectate $code" -WaitMs 500 | Out-Null }
$deadline = $start.AddSeconds($TimeoutSeconds)
$decided = 0
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 500
  $host0 = Since 0
  if ($host0 -match 'session over') { 'the session ended early'; break }
  $now = @($host0 | Where-Object { $_ -match 'the match is decided' }).Count
  if ($now -le $decided) { continue }
  $decided = $now
  Start-Sleep -Milliseconds $ChooseAfterMs
  if ($decided -gt $Choices.Count) { break }
  $c = "choose $($Choices[$decided - 1])"
  & "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands $c -WaitMs 0 | Out-Null
  & "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands $c -WaitMs 0 | Out-Null
}
Start-Sleep 2
# Mashing stays set in the game until a reload; off again for sessions played by hand.
foreach ($i in $instances) { & "$PSScriptRoot\cmd.ps1" -Instance $i -Commands 'leave; mash 0; netsim 0 0 0' -WaitMs 300 | Out-Null }
Start-Sleep 3
$pattern = 'netplay: (match [0-9a-f]{16}|match [0-9a-f]{8}:|session started as|watching match|the match is decided|after the match|both chose|back to the lobby|session over|desync detected|spectator desync|watched|.* joined from|.* watches|in .* session)'
foreach ($i in $instances) {
  "== instance $i"
  Since $i | Where-Object { $_ -match $pattern } | ForEach-Object { $_.Substring(0, 12) + ' ' + ($_ -replace '^.*?netplay: ', '').Substring(0, [Math]::Min(150, ($_ -replace '^.*?netplay: ', '').Length)) }
}
