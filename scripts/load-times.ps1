# Loads a series of battles on one instance (each 'setup ...' line, then
# battlenow) and prints how long each took from the loader start to battle
# creation, to see which characters the game still has in memory.
#   load-times.ps1 -Instance 1 -Setups 'Z41 0 1 0 1 1 KEN 0 1 0 1 1 0','RYU 0 1 0 1 1 KEN 0 1 0 1 1 0'
# -Menu goes back to the main menu between loads.
param([int]$Instance = 0, [string[]]$Setups, [switch]$Menu, [int]$TimeoutSeconds = 60)
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$log = Join-Path $w ($(if ($Instance -eq 0) { 'sfv_rollback.log' } else { "sfv_rollback.$Instance.log" }))
function Since($mark) { Get-Content $log -Tail 300 | Where-Object { $_.Length -gt 12 -and $_.Substring(0, 12) -ge $mark } }
function Stamp($line) { [datetime]::ParseExact($line.Substring(0, 12), 'HH:mm:ss.fff', $null) }
foreach ($s in $Setups) {
  if ($Menu) {
    & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $Instance -Commands 'menu' -WaitMs 8000 | Out-Null
  }
  $mark = (Get-Date).ToString('HH:mm:ss.fff')
  & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $Instance -Commands "setup $s; battlenow" -WaitMs 0 | Out-Null
  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  do {
    Start-Sleep -Milliseconds 500
    $lines = Since $mark
    $start = $lines | Where-Object { $_ -match 'starting it' } | Select-Object -First 1
    $made = $lines | Where-Object { $_ -match 'applied at battle creation' } | Select-Object -First 1
  } until (($start -and $made) -or (Get-Date) -gt $deadline)
  if ($start -and $made) { '{0}: {1:N1} s' -f $s, ((Stamp $made) - (Stamp $start)).TotalSeconds }
  else { "${s}: no battle within $TimeoutSeconds s" }
  # Let the intro start before the next jump.
  Start-Sleep 6
}
