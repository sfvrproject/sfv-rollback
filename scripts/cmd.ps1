# Writes mod commands (one per argument, or ';'-separated), waits for the mod to
# consume them plus WaitMs, then screenshots the game.
param([string]$Commands, [int]$WaitMs = 1500, [string]$Out = '', [int]$Instance = 0)
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$f = Join-Path $w ($(if ($Instance -eq 0) { 'sfv_cmd.txt' } else { "sfv_cmd.$Instance.txt" }))
# Written aside and renamed into place: the mod polls the file and must never
# see it half written (it would run nothing and delete it).
Set-Content -Path "$f.tmp" -Value ($Commands -split ';' | ForEach-Object { $_.Trim() })
Move-Item -Path "$f.tmp" -Destination $f -Force
$deadline = (Get-Date).AddSeconds(5)
while ((Test-Path $f) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
if (Test-Path $f) { 'mod did not take the command file'; exit 1 }
Start-Sleep -Milliseconds $WaitMs
if ($Out) { & (Join-Path $PSScriptRoot 'win.ps1') -Action shot -Out $Out -Instance $Instance }
