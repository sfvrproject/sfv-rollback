# Starts the game instances with the freshly built mod and waits until each
# reaches the main menu (offline boot, [boot] offline=1). -Restart closes
# running instances first; -StageSelect then walks each to Versus stage select.
# The games start detached (not children of this shell): stopping the script
# or the task that ran it leaves them running. A game that exits before the
# main menu (the occasional ntdll crash at startup) is started again, up to
# -Retries times. Fails (exit 1, error stream) when they do not all get there.
param([int]$Count = 2, [switch]$Restart, [switch]$StageSelect, [int]$StaggerSeconds = 3, [int]$TimeoutSeconds = 120,
      [int]$MenuWaitSeconds = 15, [int]$Retries = 2)
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$exe = Join-Path $w 'StreetFighterV.exe'
# A second copy needs multi_instance=1, else it quits at once (exit code 0).
if ($Count -gt 1 -and -not (Select-String -Path (Join-Path $w 'sfv_rollback.ini') -Pattern '^multi_instance=1' -Quiet)) {
  Write-Error "$Count instances need multi_instance=1 in sfv_rollback.ini (lib.ps1: Use-TestSettings)"
  exit 1
}
if ($Restart) {
  Get-Process StreetFighterV -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep 2
}
$running = @(Get-Process StreetFighterV -ErrorAction SilentlyContinue).Count
# The loader runs a copy of the DLL, so this one is never in use.
Copy-Item (Join-Path $PSScriptRoot '..\build\Release\sfv_rollback.dll') $w -Force
function Launch {
  $r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = "`"$exe`""; CurrentDirectory = $w }
  if ($r.ReturnValue -ne 0) { Write-Error "cannot start the game (Win32_Process.Create returned $($r.ReturnValue))"; exit 1 }
  [int]$r.ProcessId
}
$start = Get-Date
$mark = $start.ToString('HH:mm:ss.fff')
$pids = [System.Collections.Generic.List[int]]::new()
for ($i = $running; $i -lt $Count; $i++) {
  $pids.Add((Launch))
  if ($i + 1 -lt $Count) { Start-Sleep $StaggerSeconds }
}
# Each instance logs to SFVR\logs\sfvr[.N].log; one that started after $mark is
# up once it shows the main menu (instances already running are left alone).
$logs = @(0..($Count - 1) | ForEach-Object { Join-Path $w ($(if ($_ -eq 0) { 'SFVR\logs\sfvr.log' } else { "SFVR\logs\sfvr.$_.log" })) })
$launched = [Math]::Max(0, $Count - $running)
$deadline = $start.AddSeconds($TimeoutSeconds)
$up = @{}
$retried = 0
while ($up.Count -lt $launched -and (Get-Date) -lt $deadline) {
  Start-Sleep 1
  for ($i = 0; $i -lt $Count; $i++) {
    if ($up.ContainsKey($i) -or -not (Test-Path $logs[$i])) { continue }
    # The menu switch, then its map load (the menu's stage request, 'BLK').
    $lines = Get-Content $logs[$i] -Tail 400 | Where-Object { $_.Length -gt 12 -and $_.Substring(0, 12) -ge $mark }
    if (-not ($lines | Select-String -Quiet 'instance \d+, mode')) { continue }
    $menu = $false
    foreach ($l in $lines) {
      if ($l -match 'ChangeSceneState MainMenuState') { $menu = $true }
      elseif ($menu -and $l -match "text 'BLK'") { $up[$i] = $true; break }
    }
    if ($up.ContainsKey($i)) { "instance $i at the main menu after $([int]((Get-Date) - $start).TotalSeconds) s" }
  }
  # One that died before its menu: start another in its place.
  foreach ($p in @($pids)) {
    if (Get-Process -Id $p -ErrorAction SilentlyContinue) { continue }
    [void]$pids.Remove($p)
    if ($up.Count -ge $launched) { break }
    if ($retried -ge $Retries) { Write-Error "a game exited while starting, $retried retries used"; exit 1 }
    $retried++
    "a game (pid $p) exited while starting; starting another ($retried of $Retries)"
    $pids.Add((Launch))
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  }
}
if ($up.Count -lt $launched) {
  Write-Error "not all started instances reached the main menu in $TimeoutSeconds s"
  exit 1
}
if ($StageSelect) {
  # The menu ignores input for a while after its map has loaded.
  Start-Sleep $MenuWaitSeconds
  $c = (Get-Content (Join-Path $PSScriptRoot 'nav_menu.txt')) -join ';'
  for ($i = 0; $i -lt $Count; $i++) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $i -Commands $c -WaitMs 100 }
  Start-Sleep 20
  'at stage select (check with win.ps1 -Action shot)'
}
exit 0
