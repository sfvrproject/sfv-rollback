# Writes mod commands (one per argument, or ';'-separated) for a game instance.
# The mod takes the file at once and runs its steps in order, after any still
# queued. Without -Wait this returns once the file is taken (plus WaitMs);
# with -Wait once every step has run (an echo marker shows in the log).
# Failures go to the error stream with exit code 1, so a caller that pipes
# the output away (| Out-Null) still sees them (lib.ps1's Send-Cmd throws).
param([string]$Commands, [int]$WaitMs = 1500, [string]$Out = '', [int]$Instance = 0, [switch]$Wait,
      [int]$TimeoutSeconds = 30)
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$f = Join-Path $w ($(if ($Instance -eq 0) { 'sfv_cmd.txt' } else { "sfv_cmd.$Instance.txt" }))
if (-not (Get-Process StreetFighterV -ErrorAction SilentlyContinue)) {
  Write-Error "no game is running (commands for instance $Instance)"
  exit 1
}
# An earlier file not taken yet is waited for, never replaced (its commands
# would be lost).
$deadline = (Get-Date).AddSeconds(10)
while ((Test-Path $f) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
if (Test-Path $f) {
  Write-Error "instance $Instance has not taken its previous command file for 10 s (is it running? stuck?)"
  exit 1
}
$lines = @($Commands -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
if ($Wait) {
  . (Join-Path $PSScriptRoot 'logtail.ps1')
  $log = Join-Path $w ($(if ($Instance -eq 0) { 'SFVR\logs\sfvr.log' } else { "SFVR\logs\sfvr.$Instance.log" }))
  $tail = New-LogTail $log
  $marker = 'done-' + [guid]::NewGuid().ToString('N').Substring(0, 10)
  $lines += "echo $marker"
}
# Written aside (as UTF-8: names may be in any script) and renamed into
# place: the mod polls the file and must never see it half written.
[IO.File]::WriteAllLines("$f.tmp", [string[]]$lines, [Text.UTF8Encoding]::new($false))
Move-Item -Path "$f.tmp" -Destination $f -Force
$deadline = (Get-Date).AddSeconds(5)
while ((Test-Path $f) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
if (Test-Path $f) {
  Write-Error "instance $Instance did not take the command file within 5 s"
  exit 1
}
if ($Wait) {
  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  $done = $false
  while (-not $done -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
    Update-LogTail $tail
    $done = [bool]($tail.Lines | Select-String -SimpleMatch "cmd: echo $marker" -Quiet)
  }
  if (-not $done) {
    Write-Error "instance ${Instance}: the commands did not finish within $TimeoutSeconds s"
    exit 1
  }
}
Start-Sleep -Milliseconds $WaitMs
if ($Out) { & (Join-Path $PSScriptRoot 'win.ps1') -Action shot -Out $Out -Instance $Instance }
exit 0
