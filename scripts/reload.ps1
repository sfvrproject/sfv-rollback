# Copies the freshly built mod next to the game and asks the loader to reload it.
param([int[]]$Instances = @(0))
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$b = (Join-Path $PSScriptRoot '..\build\Release')
Copy-Item "$b\sfv_rollback.dll" $w -Force
foreach ($i in $Instances) {
  $trig = Join-Path $w ($(if ($i -eq 0) { 'sfv_reload' } else { "sfv_reload.$i" }))
  New-Item -ItemType File -Path $trig -Force | Out-Null
  $deadline = (Get-Date).AddSeconds(10)
  while ((Test-Path $trig) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
}
Start-Sleep -Milliseconds 1500
Get-Content (Join-Path $w 'sfv_rollback.log') -Tail 8
