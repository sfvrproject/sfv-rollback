# Local battle on one instance, pressing a button (-Button, default a) through the intro on the chosen
# virtual pads (none, 0, 1, both at once, or alt: one after the other), and prints when each round flow event
# came, from the battle's start (event 1) to FIGHT (event 6).
#   intro-skip.ps1 -Instance 1 -Pads none,0,1,both
param([int]$Instance = 1, [string[]]$Pads = @('none', '0', '1', 'both'), [int]$PressSeconds = 14,
      [string]$Setup = 'RYU 0 1 0 1 1 KEN 0 1 0 1 1 0', [int]$StartDelayMs = 0,
      [string]$Button = 'a')
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$log = Join-Path $w ($(if ($Instance -eq 0) { 'SFVR\logs\sfvr.log' } else { "SFVR\logs\sfvr.$Instance.log" }))
function Since($mark) { Get-Content $log -Tail 400 | Where-Object { $_.Length -gt 12 -and $_.Substring(0, 12) -ge $mark } }
function Stamp($line) { [datetime]::ParseExact($line.Substring(0, 12), 'HH:mm:ss.fff', $null) }
foreach ($mode in $Pads) {
  $mark = (Get-Date).ToString('HH:mm:ss.fff')
  & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $Instance -Commands "setup $Setup; battlenow" -WaitMs 0 | Out-Null
  $deadline = (Get-Date).AddSeconds(60)
  do { Start-Sleep -Milliseconds 100 } until ((Since $mark) -match 'round flow event 1$' -or (Get-Date) -gt $deadline)
  if ($StartDelayMs) { Start-Sleep -Milliseconds $StartDelayMs }
  if ($mode -ne 'none') {
    $one = switch ($mode) { '0' { "pad 0 $Button 80 220" } '1' { "pad 1 $Button 80 220" } 'alt' { "pad 0 $Button 80 60;pad 1 $Button 80 160" } default { "pad both $Button 80 220" } }
    $steps = (1..[int]($PressSeconds / 0.3) | ForEach-Object { $one }) -join ';'
    & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $Instance -Commands $steps -WaitMs 0 | Out-Null
  }
  $deadline = (Get-Date).AddSeconds(40)
  do { Start-Sleep -Milliseconds 250 } until ((Since $mark) -match 'round flow event 6$' -or (Get-Date) -gt $deadline)
  $events = Since $mark | Where-Object { $_ -match 'round flow event (\d+)$' }
  $t0 = $events | Where-Object { $_ -match 'event 1$' } | Select-Object -First 1
  $line = foreach ($e in $events) { if ($e -match 'event (\d+)$') { '{0}@{1:N1}' -f $Matches[1], ((Stamp $e) - (Stamp $t0)).TotalSeconds } }
  "${mode}: $($line -join ' ')"
  Start-Sleep 2
}
