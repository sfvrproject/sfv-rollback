# Publishes a release to the project site (the sfvr-site repository next to
# this one) for the updater (src/mod/updater.h):
#   release.ps1
#     builds, runs the unit tests, copies the files into
#     sfvr-site\public\releases\<version>\ (and the full package for first
#     installs), then writes public\update.json and signs it into
#     public\update.json.sig. The version is kModVersion in
#     src\mod\mod_main.cpp: raise it first, and give it a section in
#     CHANGELOG.md, whose first paragraph becomes the notes players see
#     (-Notes overrides them).
#   release.ps1 -NewKey
#     makes the update signing key, once: the private half goes to -KeyFile
#     (outside the repositories: keep a backup, never share it), the public
#     half into src\mod\update_key.h. Builds made after that install only
#     updates signed with it.
# Then commit here and in sfvr-site, tag this repository v<version>, and
# build the site's upload (sfvr-site\build.ps1).
# Needs PowerShell 7 (for the key's PKCS#8 import and export).
param([string]$Notes = '', [string]$Site = (Join-Path $PSScriptRoot '..\..\sfvr-site'),
      [string]$KeyFile = (Join-Path $env:USERPROFILE '.sfvr\update-signing-key.txt'),
      [switch]$NewKey, [switch]$Force, [switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$build = Join-Path $root 'build\Release'
$keyHeader = Join-Path $root 'src\mod\update_key.h'

function PublicKeyOf($ecdsa) {
  $q = $ecdsa.ExportParameters($false).Q
  return [byte[]]($q.X + $q.Y)
}

if ($NewKey) {
  if ((Test-Path $KeyFile) -and -not $Force) { throw "$KeyFile exists; -Force replaces it (and every build made since stops taking updates)" }
  $ecdsa = [System.Security.Cryptography.ECDsa]::Create([System.Security.Cryptography.ECCurve+NamedCurves]::nistP256)
  New-Item -ItemType Directory -Force (Split-Path $KeyFile) | Out-Null
  [Convert]::ToBase64String($ecdsa.ExportPkcs8PrivateKey()) | Set-Content $KeyFile -NoNewline
  $hex = (PublicKeyOf $ecdsa | ForEach-Object { '0x{0:x2}' -f $_ })
  $rows = for ($i = 0; $i -lt 64; $i += 16) { '    ' + ($hex[$i..($i + 15)] -join ', ') + ',' }
  $text = Get-Content $keyHeader -Raw
  $array = "constexpr uint8_t kUpdatePublicKey[64] = {`n" + ($rows -join "`n") + "`n};"
  $text = [regex]::Replace($text, 'constexpr uint8_t kUpdatePublicKey\[64\] = \{[^;]*\};', $array.Replace('$', '$$'))
  $text = $text.Replace("// private half stays with whoever publishes releases. All zeros: this build`n// installs no updates.",
                        "// private half stays with whoever publishes releases.")
  [IO.File]::WriteAllText($keyHeader, $text)
  "key written to $KeyFile (back it up); public half in src\mod\update_key.h: rebuild"
  exit 0
}

# The key, and that the source has its public half.
if (-not (Test-Path $KeyFile)) { throw "no signing key at $KeyFile (release.ps1 -NewKey makes one)" }
$ecdsa = [System.Security.Cryptography.ECDsa]::Create()
$read = 0
$ecdsa.ImportPkcs8PrivateKey([Convert]::FromBase64String((Get-Content $KeyFile -Raw)), [ref]$read)
$built = [regex]::Matches((Get-Content $keyHeader -Raw), '0x([0-9a-fA-F]{2})') | ForEach-Object { [Convert]::ToByte($_.Groups[1].Value, 16) }
if ((@($built) -join ',') -ne ((PublicKeyOf $ecdsa) -join ',')) { throw "src\mod\update_key.h does not hold $KeyFile's public key" }

$version = (Select-String -Path (Join-Path $root 'src\mod\mod_main.cpp') -Pattern 'kModVersion = "([^"]+)"').Matches.Groups[1].Value
$public = Join-Path (Resolve-Path $Site) 'public'
$release = Join-Path $public "releases\$version"
if ((Test-Path $release) -and -not $Force) { throw "release $version is already on the site ($release); raise kModVersion" }

# The version's changelog entry; its first paragraph is the players' notes.
$changelog = Get-Content (Join-Path $root 'CHANGELOG.md') -Raw
$entry = [regex]::Match($changelog, "(?ms)^## \[$([regex]::Escape($version))\][^\n]*\n(.*?)(?=^## \[|\z)")
if (-not $entry.Success) { throw "CHANGELOG.md has no section for $version" }
if (-not $Notes) {
  $Notes = (($entry.Groups[1].Value.Trim() -split "\r?\n\s*\r?\n")[0] -replace '\s*\r?\n\s*', ' ').Trim()
  if ($Notes.StartsWith('#')) { throw "the $version section in CHANGELOG.md starts without a summary paragraph" }
}

if (-not $SkipBuild) {
  $vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
  cmd /c "`"$vcvars`" >nul 2>nul && cmake --build `"$build`""
  if ($LASTEXITCODE) { throw 'the build failed' }
  & (Join-Path $build 'sfvtests.exe') | Select-Object -Last 1
  if ($LASTEXITCODE) { throw 'the unit tests failed' }
}

# The files the updater installs, and the package for first installs.
New-Item -ItemType Directory -Force $release | Out-Null
$sources = [ordered]@{
  'xinput1_3.dll'    = Join-Path $build 'xinput1_3.dll'
  'sfv_rollback.dll' = Join-Path $build 'sfv_rollback.dll'
  'sfv_noise.txt'    = Join-Path $root 'data\sfv_noise.txt'
}
$files = foreach ($name in $sources.Keys) {
  Copy-Item $sources[$name] (Join-Path $release $name) -Force
  $item = Get-Item (Join-Path $release $name)
  [ordered]@{ path = $name; size = $item.Length; sha256 = (Get-FileHash $item -Algorithm SHA256).Hash.ToLower(); url = "releases/$version/$name" }
}
& (Join-Path $PSScriptRoot 'package.ps1') -Public | Out-Null
Copy-Item (Join-Path $root "dist\sfvr-$version.zip") $release -Force
# The symbols stay here, for reading crash addresses (scripts/symbolize.py).
$symbols = New-Item -ItemType Directory -Force (Join-Path $root "dist\symbols\$version")
Copy-Item (Join-Path $build 'sfv_rollback.pdb') $symbols -Force

$info = [ordered]@{
  product   = 'SFVR'
  version   = $version
  published = (Get-Date -Format 'yyyy-MM-dd')
  notes     = $Notes
  download  = "releases/$version/sfvr-$version.zip"
  files     = @($files)
}
$bytes = [Text.UTF8Encoding]::new($false).GetBytes(($info | ConvertTo-Json -Depth 4) + "`n")
$signature = $ecdsa.SignData($bytes, [Security.Cryptography.HashAlgorithmName]::SHA256)
[IO.File]::WriteAllBytes((Join-Path $public 'update.json'), $bytes)
[IO.File]::WriteAllText((Join-Path $public 'update.json.sig'), [Convert]::ToBase64String($signature))
"SFVR $version published to $public ($($files.Count) files, signed)"
"notes: $Notes"
"next: commit here and in sfvr-site, git tag v$version, then sfvr-site\build.ps1 for the upload"
