# Netplay soak test between two local game instances (sfv 0 and sfv 1).
# Both start in local Versus (a battle, or stage select). Each round: random
# simulated network conditions per side, random host, random match setup
# ("host random": characters, settings and stage), both players mash, play for
# $Seconds, leave (both sides save their recording), and diff the recordings.
# The next round jumps straight from the running battle into the new one.
# -Delay: both players' input delay (default 0: the most rollbacks).
param([int]$Rounds = 5, [int]$Seconds = 45, [int]$Seed = 1, [switch]$Costumes, [switch]$Clean,
      [int]$MaxLatency = 90, [int]$MaxJitter = 30, [int]$MaxLoss = 8, [int]$Delay = 0)

$w = (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')
$sfvr = (Join-Path $PSScriptRoot '..\build\Release\sfvr.exe')
$rng = New-Object System.Random $Seed
$results = @()

function Log($i) { if ($i -eq 0) { Join-Path $w 'sfv_rollback.log' } else { Join-Path $w "sfv_rollback.$i.log" } }
function Cmd($i, $text) {
    $f = if ($i -eq 0) { Join-Path $w 'sfv_cmd.txt' } else { Join-Path $w "sfv_cmd.$i.txt" }
    Set-Content -Path $f -Value ($text -split ';' | ForEach-Object { $_.Trim() })
    $deadline = (Get-Date).AddSeconds(5)
    while ((Test-Path $f) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 50 }
}
function LinesSince($i, $mark) {
    Get-Content (Log $i) | Where-Object { $_.Length -ge 12 -and $_.Substring(0, 12) -ge $mark }
}
function Grab($lines, $pattern) {
    $m = $lines | Select-String $pattern | Select-Object -Last 1
    if ($m) { $m.Matches.Groups[1].Value } else { '' }
}

for ($r = 1; $r -le $Rounds; $r++) {
    $hostI = $rng.Next(0, 2); $joinI = 1 - $hostI
    $net = @(0, 1 | ForEach-Object {
        if ($Clean) { @{ lat = 0; jit = 0; loss = 0 } }
        else { @{ lat = $rng.Next(0, $MaxLatency + 1); jit = $rng.Next(0, $MaxJitter + 1); loss = $rng.Next(0, $MaxLoss + 1) } } })
    $mark = (Get-Date).ToString('HH:mm:ss.fff')
    "round ${r}: host sfv $hostI | sfv0 sends $($net[0].lat)+-$($net[0].jit) ms $($net[0].loss)% loss | sfv1 sends $($net[1].lat)+-$($net[1].jit) ms $($net[1].loss)% loss"

    $random = if ($Costumes) { 'host random costumes' } else { 'host random' }
    Cmd $hostI "delay $Delay; mash $($rng.Next(1, 100000)); $random; netsim $($net[$hostI].lat) $($net[$hostI].jit) $($net[$hostI].loss)"
    Start-Sleep -Milliseconds 800
    $code = Grab (LinesSince $hostI $mark) 'hosting, code (\S+)'
    if (-not $code) { "  no code from the host"; break }
    Cmd $joinI "delay $Delay; mash $($rng.Next(1, 100000)); join $code; netsim $($net[$joinI].lat) $($net[$joinI].jit) $($net[$joinI].loss)"

    # Both jump into the battle, load and start the session.
    $deadline = (Get-Date).AddSeconds(150)
    do {
        Start-Sleep -Seconds 2
        $l0 = LinesSince 0 $mark; $l1 = LinesSince 1 $mark
        $started = @($l0, $l1 | Where-Object { $_ | Select-String 'session started as' }).Count
        $failed = ($l0 + $l1) | Select-String 'battles differ|no battle after|cannot start|could not reach' | Select-Object -First 1
    } while ($started -lt 2 -and -not $failed -and (Get-Date) -lt $deadline)
    $setup = Grab (LinesSince $hostI $mark) 'player joined from \S+; (.*)$'
    "  setup: $setup"
    if ($started -lt 2) {
        "  did not start: $(if ($failed) { $failed.Line } else { 'timeout' })"
        $results += [pscustomobject]@{ round = $r; setup = $setup; result = 'no start' }
        Cmd 0 'leave'; Cmd 1 'leave'; Start-Sleep 2
        continue
    }
    Start-Sleep -Seconds $Seconds
    Cmd 0 'netstatus'; Cmd 1 'netstatus'
    Start-Sleep -Milliseconds 500
    # Mashing and conditions stay set until a reload (the next round sets its own).
    Cmd 0 'leave; mash 0; netsim 0 0 0'; Cmd 1 'leave; mash 0; netsim 0 0 0'
    Start-Sleep -Seconds 2

    $row = [pscustomobject]@{ round = $r; setup = $setup; net = "$($net[0].lat)/$($net[0].jit)/$($net[0].loss) $($net[1].lat)/$($net[1].jit)/$($net[1].loss)" }
    $files = @()
    foreach ($i in 0, 1) {
        $lines = LinesSince $i $mark
        $row | Add-Member "frozen$i" (Grab $lines 'checksum ([0-9A-F]{8})\); frozen')
        $row | Add-Member "desync$i" ($(Grab $lines 'desync detected at frame (\d+)'))
        $row | Add-Member "status$i" (Grab $lines 'netplay status: (.*)$')
        $f = Grab $lines 'recording (\S+\.sfvr)'
        if ($f) { $files += $f }
    }
    "  sfv0: frozen $($row.frozen0) first desync '$($row.desync0)' | $($row.status0)"
    "  sfv1: frozen $($row.frozen1) first desync '$($row.desync1)' | $($row.status1)"
    if ($files.Count -eq 2) {
        $diff = & $sfvr diff $files[0] $files[1] 2>&1
        $row | Add-Member result (($diff | Select-String '^result|^state diverges' | Select-Object -First 1).Line)
        "  diff: $($row.result)"
        $peers = & $sfvr peers $files[0] $files[1] 2>&1
        $peers | Select-String '^frame' | ForEach-Object { "    $($_.Line)" }
    }
    $results += $row
    Start-Sleep -Seconds 2
}
$results | Format-Table round, net, frozen0, frozen1, desync0, desync1, result -AutoSize | Out-String -Width 300
