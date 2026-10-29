# The post-match flow: instance 0 hosts, 1 joins, both mash through a match.
# At the decision nobody chooses for -HoldSeconds (the game's own results
# menu must stay closed: "results screen held"), screenshots of both games
# are taken, then both choose Rematch; the rematch's first frames are shot
# too (its stage must be the chosen one), then both leave at the second
# decision. Prints the timings from the logs: decision -> held, choice ->
# rematch running.
param([int]$HoldSeconds = 40, [string]$Stage = 'CHN', [int[]]$Seeds = @(11, 22), [string]$Shots = (Join-Path $env:TEMP 'sfv-rematch-test'))
$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
function LogOf($i) { Join-Path $w ($(if ($i -eq 0) { 'SFVR\logs\sfvr.log' } else { "SFVR\logs\sfvr.$i.log" })) }
$n = @{}; foreach ($i in 0, 1) { $n[$i] = @(Get-Content (LogOf $i)).Count }
function Since($i) { Get-Content (LogOf $i) | Select-Object -Skip $n[$i] }
function WaitCount($i, $pattern, $count, $seconds) {
  $end = (Get-Date).AddSeconds($seconds)
  while ((Get-Date) -lt $end) { if (@(Since $i | Where-Object { $_ -match $pattern }).Count -ge $count) { return $true }; Start-Sleep -Milliseconds 500 }
  "timed out waiting for $count x '$pattern' on $i"
  $false
}
function Shot($name) { foreach ($i in 0, 1) { & (Join-Path $PSScriptRoot 'win.ps1') -Action shot -Instance $i -Out (Join-Path $Shots "$name-$i.png") | Out-Null } }
New-Item -ItemType Directory -Force -Path $Shots | Out-Null
& (Join-Path $PSScriptRoot 'weyve-server.ps1') | Out-Null
& (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 0 -Commands "mash $($Seeds[0]); dumps 600 24; pick RYU 0 1 0 1 1; stage $Stage; host" -WaitMs 1500 | Out-Null
$code = (Since 0 | Select-String 'hosting, code (\S+)' | Select-Object -Last 1).Matches.Groups[1].Value
& (Join-Path $PSScriptRoot 'cmd.ps1') -Instance 1 -Commands "mash $($Seeds[1]); dumps 600 24; pick KEN 0 1 0 1 1; join $code" -WaitMs 500 | Out-Null
if (-not (WaitCount 0 'match running' 1 120)) { exit 1 }
Start-Sleep 20
Shot 'match1'
if (-not (WaitCount 0 'the match is decided' 1 400)) { exit 1 }
Start-Sleep 3
Shot 'decided'
Start-Sleep $HoldSeconds
Shot 'held'
foreach ($i in 0, 1) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $i -Commands 'choose rematch' -WaitMs 0 | Out-Null }
WaitCount 0 'match running' 2 120 | Out-Null
Start-Sleep 8
Shot 'rematch'
WaitCount 0 'the match is decided' 2 400 | Out-Null
Start-Sleep 3
foreach ($i in 0, 1) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $i -Commands 'leave' -WaitMs 300 | Out-Null }
Start-Sleep 3
$pattern = 'netplay: (match [0-9a-f]{8}:|session started as|the match is decided|both chose|jumping into|battle created|battle objects ready|match running|session over)|results screen held|round flow event (7|11|12|13|14)|battle setup at battle start|desync detected'
foreach ($i in 0, 1) {
  "== instance $i"
  Since $i | Where-Object { $_ -match $pattern } | ForEach-Object { $_.Substring(0, [Math]::Min(190, $_.Length)) }
}
"screenshots in $Shots"
