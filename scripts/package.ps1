# Packs this build for another PC: dist\sfvr-<version>.zip (SFVR) with
#   Win64\          what goes next to StreetFighterV.exe (StreetFighterV\Binaries\Win64):
#                   the loader, the mod, the noise mask, an ini
#   sfv-rollback\   what goes into the game's folder for tests (the scripts and
#                   the build's DLL and tools, where the scripts expect them)
# -Server sets [weyve] server in the ini (default: the public server); -Testing also turns on the virtual pads
# and background input the test scripts need (mashing, the agent).
#   package.ps1 -Server 192.168.1.10 -Testing
param([string]$Server = '159.69.32.141:5555', [switch]$Testing)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $root 'build\Release'
$version = (Select-String -Path (Join-Path $root 'src\mod\mod_main.cpp') -Pattern 'kModVersion = "([^"]+)"').Matches.Groups[1].Value
$name = "sfvr-$version"
$out = Join-Path $root "dist\$name"
if (Test-Path $out) { Remove-Item $out -Recurse -Force }
$win = New-Item -ItemType Directory -Force (Join-Path $out 'Win64')
$repo = New-Item -ItemType Directory -Force (Join-Path $out 'sfv-rollback')

Copy-Item (Join-Path $build 'xinput1_3.dll'), (Join-Path $build 'sfv_rollback.dll') $win
Copy-Item (Join-Path $root 'data\sfv_noise.txt') $win
$ini = Get-Content (Join-Path $root 'sfv_rollback.ini.example')
$serverAddress = if ($Server -match ':') { $Server } else { "${Server}:5555" }
$ini = $ini -replace '^server=.*$', "server=$serverAddress"
if ($Testing) {
  $ini = $ini -replace '^virtual_pads=.*$', 'virtual_pads=1' -replace '^always_active=.*$', 'always_active=1'
}
Set-Content (Join-Path $win 'sfv_rollback.ini') $ini

New-Item -ItemType Directory -Force (Join-Path $repo 'build\Release'), (Join-Path $repo 'data') | Out-Null
Copy-Item (Join-Path $root 'scripts') $repo -Recurse
foreach ($f in 'sfv_rollback.dll', 'xinput1_3.dll', 'sfvr.exe', 'weyve_server.exe') { Copy-Item (Join-Path $build $f) (Join-Path $repo 'build\Release') }
Copy-Item (Join-Path $root 'data\sfv_noise.txt') (Join-Path $repo 'data')

@"
$name

1. Copy the files in Win64\ into StreetFighterV\StreetFighterV\Binaries\Win64\
   (next to StreetFighterV.exe).
2. For tests: copy the sfv-rollback\ folder into the game's folder
   (StreetFighterV\sfv-rollback\), so its scripts find the game.
3. Start the game from Steam as usual. F1 opens the panel.

Server: $serverAddress (sfv_rollback.ini, [weyve] server).
"@ | Set-Content (Join-Path $out 'INSTALL.txt')

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $out '*') -DestinationPath $zip
"$zip ($([int]((Get-Item $zip).Length / 1KB)) KB)"
