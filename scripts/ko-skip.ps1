# Netplay match (instance 0 hosts, 1 joins) with both sides mashing; at every
# round end (flow event 7) and at the match's end (11) Start is pressed on the
# chosen sides (none, host, join, both). Stops at the results screen (13) or
# the session's end, then both leave. Prints per side and round end the time
# to each later flow event, and any desync.
#   ko-skip.ps1 -Press both
param([ValidateSet('none', 'host', 'join', 'both')][string]$Press = 'both', [int]$PressAfterMs = 1000,
      [string]$Stage = 'CHN', [int]$TimeoutSeconds = 600, [int]$PressCount = 25,
      [switch]$UiAtMatchEnd, [switch]$UiStartAtMatchEnd, [string]$UiButton = 'start')
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$logs = @((Join-Path $w 'sfv_rollback.log'), (Join-Path $w 'sfv_rollback.1.log'))
function Since($log, $mark) { Get-Content $log -Tail 8000 | Where-Object { $_.Length -gt 12 -and $_.Substring(0, 12) -ge $mark } }
function Stamp($line) { [datetime]::ParseExact($line.Substring(0, 12), 'HH:mm:ss.fff', $null) }
$ended = 'netplay: (match over|the other player left)'
$mark = (Get-Date).ToString('HH:mm:ss.fff')
& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands "mash 11; stage $Stage; host" -WaitMs 800 | Out-Null
$code = (Select-String -Path $logs[0] -Pattern 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
& "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands "mash 22; join $code" -WaitMs 100 | Out-Null
$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$pressed = 0
$presses = (1..$PressCount | ForEach-Object { 'pad 0 start 80 250' }) -join ';'
while ((Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 300
  $host0 = Since $logs[0] $mark
  if (($host0 -match $ended) -or ($host0 -match 'round flow event 13$')) { break }
  $ends = @($host0 | Where-Object { $_ -match 'round flow event (7|11)$' }).Count
  if ($ends -gt $pressed) {
    $pressed = $ends
    Start-Sleep -Milliseconds $PressAfterMs
    # -UiAtMatchEnd: at the match's end the pressing sides' games see their pads (testing).
    # -UiStartAtMatchEnd: at the match's end Start goes straight to the game's pad input (testing).
    $c = if ($UiAtMatchEnd -and ($host0 -match 'round flow event 11$')) { 'uipads 1;' + $presses }
         elseif ($UiStartAtMatchEnd -and ($host0 -match 'round flow event 11$')) { (1..$PressCount | ForEach-Object { "uistart 150 $UiButton;wait 250" }) -join ';' }
         else { $presses }
    if ($Press -in 'host', 'both') { & "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands $c -WaitMs 0 | Out-Null }
    if ($Press -in 'join', 'both') { & "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands $c -WaitMs 0 | Out-Null }
  }
}
Start-Sleep 2
& "$PSScriptRoot\cmd.ps1" -Instance 0 -Commands 'leave' -WaitMs 300 | Out-Null
& "$PSScriptRoot\cmd.ps1" -Instance 1 -Commands 'leave' -WaitMs 3000 | Out-Null
foreach ($i in 0, 1) {
  $lines = Since $logs[$i] $mark
  $events = $lines | Where-Object { $_ -match "round flow event (\d+)$|$ended" }
  $out = @(); $t0 = $null
  foreach ($e in $events) {
    if ($e -match 'event 7$') { $t0 = Stamp $e; $out += '| 7' ; continue }
    if (-not $t0) { continue }
    $name = if ($e -match 'event (\d+)$') { $Matches[1] } else { 'end' }
    $out += '{0}@{1:N1}' -f $name, ((Stamp $e) - $t0).TotalSeconds
  }
  $skips = @($lines -match 'both players pressed Start').Count
  $desync = ($lines -match 'desync detected' | Select-Object -First 1)
  "instance ${i}: $($out -join ' ') | skips logged: $skips | desync: $(if ($desync) { $desync.Substring(21) } else { 'no' })"
}
