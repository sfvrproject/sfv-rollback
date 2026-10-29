# Starts the Weyvelength server built with the mod (build\Release\weyve_server.exe)
# on this PC for local tests, unless one already runs. Its log goes next to the
# game's logs (weyve_server.log). -Stop ends it.
param([int]$Port = 5555, [switch]$Stop)
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$running = Get-Process weyve_server -ErrorAction SilentlyContinue
if ($Stop) {
  $running | Stop-Process -Force
  'stopped'
  exit 0
}
if ($running) { "already running (pid $($running.Id -join ', '))"; exit 0 }
$exe = Join-Path $PSScriptRoot '..\build\Release\weyve_server.exe'
$p = Start-Process -FilePath $exe -ArgumentList '--port', $Port, '--room-code-length', 6 -WindowStyle Hidden -PassThru `
  -RedirectStandardOutput (Join-Path $w 'weyve_server.log') -RedirectStandardError (Join-Path $w 'weyve_server.err')
Start-Sleep -Milliseconds 500
if ($p.HasExited) { 'the server exited at once; see weyve_server.err'; exit 1 }
"weyve_server on port $Port (pid $($p.Id))"
