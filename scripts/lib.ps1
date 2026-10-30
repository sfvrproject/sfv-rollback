# Shared test helpers (dot-source it: . "$PSScriptRoot\lib.ps1"). Every helper
# throws when something does not happen, instead of the script going on with
# a stale value (an earlier room code, a command the game never took, a game
# that crashed while starting):
#   Start-Games -Count 2 [-Restart]          up.ps1, failing loudly
#   $t = New-GameTail 0                     the instance's log from now on
#   Send-Cmd 0 'mash 11; host' [-Wait]      cmd.ps1; -Wait: until the steps ran
#   $m = Wait-Log $t 'hosting, code (\S+)' 30   the first new matching line
#   $code = Start-Host 0 'mash 11; stage CHN'   host (after these commands), its room code
#   Start-Join 1 $code 'mash 22'            join, waits for the session to start
#   Use-TestSettings -Instances 2 { ... }   test ini values, restored afterwards
#   Reset-Instances 0,1 [-Server addr]      leave any session, mashing and netsim off
. (Join-Path $PSScriptRoot 'logtail.ps1')
$script:GameDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')).Path
$script:IniPath = Join-Path $script:GameDir 'sfv_rollback.ini'

function LogOf([int]$Instance) {
  Join-Path $script:GameDir ($(if ($Instance -eq 0) { 'SFVR\logs\sfvr.log' } else { "SFVR\logs\sfvr.$Instance.log" }))
}

function Assert-GamesRunning([int]$Count = 1) {
  $n = @(Get-Process StreetFighterV -ErrorAction SilentlyContinue).Count
  if ($n -lt $Count) { throw "$n game(s) running, $Count needed (did one crash? see the Windows Application log)" }
}

function Start-Games([int]$Count = 2, [switch]$Restart) {
  $out = & (Join-Path $PSScriptRoot 'up.ps1') -Count $Count -Restart:$Restart 2>&1
  if ($LASTEXITCODE) { throw "starting the games failed: $($out -join '; ')" }
  $out
}

function New-GameTail([int]$Instance) {
  $t = New-LogTail (LogOf $Instance)
  $t | Add-Member -NotePropertyName Instance -NotePropertyValue $Instance
  $t
}

# The first line added since the tail was made (or since the last match) that
# matches; throws after $Seconds or when no game is running any more.
function Wait-Log($Tail, [string]$Pattern, [int]$Seconds = 60, [int]$Games = 1) {
  if (-not $Tail.PSObject.Properties['Seen']) { $Tail | Add-Member -NotePropertyName Seen -NotePropertyValue 0 }
  $deadline = (Get-Date).AddSeconds($Seconds)
  while ($true) {
    Update-LogTail $Tail
    for ($k = $Tail.Seen; $k -lt $Tail.Lines.Count; $k++) {
      $m = [regex]::Match($Tail.Lines[$k], $Pattern)
      if ($m.Success) { $Tail.Seen = $k + 1; return $m }
    }
    $Tail.Seen = $Tail.Lines.Count
    if ((Get-Date) -gt $deadline) { throw "instance $($Tail.Instance): no '$Pattern' within $Seconds s" }
    Assert-GamesRunning $Games
    Start-Sleep -Milliseconds 250
  }
}

function Send-Cmd([int]$Instance, [string]$Commands, [switch]$Wait, [int]$Seconds = 30) {
  $out = & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $Instance -Commands $Commands -WaitMs 0 -Wait:$Wait -TimeoutSeconds $Seconds 2>&1
  if ($LASTEXITCODE) { throw "instance ${Instance}: '$Commands' failed: $($out -join '; ')" }
}

# Hosts on the instance after $Before (picks, mashing, stage, server) with
# $Command ('host', 'host manual', 'host random') and returns the new room
# code: never an earlier one from the log.
function Start-Host([int]$Instance, [string]$Before = '', [int]$Seconds = 30, [string]$Command = 'host') {
  $t = New-GameTail $Instance
  Send-Cmd $Instance ($(if ($Before) { "$Before; " } else { '' }) + $Command)
  $m = Wait-Log $t 'hosting, code (\S+)|session over: (.*)' $Seconds
  if (-not $m.Groups[1].Success) { throw "instance ${Instance}: hosting failed: $($m.Groups[2].Value)" }
  $m.Groups[1].Value
}

# Joins (or with -Spectate watches) and waits for the session (or the
# watching) to start; throws on "session over" first.
function Start-Join([int]$Instance, [string]$Code, [string]$Before = '', [switch]$Spectate, [int]$Seconds = 150, [switch]$NoWait) {
  $t = New-GameTail $Instance
  $verb = if ($Spectate) { 'spectate' } else { 'join' }
  Send-Cmd $Instance ($(if ($Before) { "$Before; " } else { '' }) + "$verb $Code")
  if ($NoWait) { return $t }
  $started = if ($Spectate) { 'in .* session as a spectator' } else { 'session started as' }
  $m = Wait-Log $t "$started|session over: (.*)" $Seconds
  if ($m.Groups[1].Success) { throw "instance ${Instance}: ${verb} failed: $($m.Groups[1].Value)" }
  $t
}

function Start-RoomsServer([int]$Port = 5555) {
  $out = & (Join-Path $PSScriptRoot 'weyve-server.ps1') -Port $Port 2>&1
  if ($LASTEXITCODE) { throw "the rooms server did not start: $($out -join '; ')" }
}

function Reset-Instances([int[]]$Instances, [string]$Server = '') {
  foreach ($i in $Instances) {
    $c = 'leave; mash 0; netsim 0 0 0' + $(if ($Server) { "; server $Server" } else { '' })
    Send-Cmd $i $c -Wait
  }
}

# Runs $Body with the ini set for tests (virtual pads, always active, and
# with more than one instance multi_instance); the player's values come back
# afterwards, also when $Body throws.
function Use-TestSettings([int]$Instances = 1, [scriptblock]$Body) {
  $want = @{ virtual_pads = '1'; always_active = '1' }
  if ($Instances -gt 1) { $want.multi_instance = '1' }
  $before = Get-Content $script:IniPath
  $after = foreach ($line in $before) {
    $k = ($line -split '=', 2)[0].Trim()
    if ($want.ContainsKey($k)) { "$k=$($want[$k])" } else { $line }
  }
  [IO.File]::WriteAllLines($script:IniPath, [string[]]$after)
  try { & $Body }
  finally {
    # Only the keys set here go back (the game may have written others meanwhile).
    $now = Get-Content $script:IniPath
    $old = @{}
    foreach ($line in $before) { $k = ($line -split '=', 2)[0].Trim(); if ($want.ContainsKey($k)) { $old[$k] = $line } }
    $restored = foreach ($line in $now) {
      $k = ($line -split '=', 2)[0].Trim()
      if ($old.ContainsKey($k)) { $old[$k] } else { $line }
    }
    [IO.File]::WriteAllLines($script:IniPath, [string[]]$restored)
  }
}
