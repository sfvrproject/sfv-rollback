# Test agent for a second PC: lets scripts on the main PC drive the game on
# this one (two-machine tests, scripts\remote.ps1). Run it here, next to the
# game, from the sfv-rollback folder copied into the game folder (the same
# layout as on the main PC):
#   powershell -ExecutionPolicy Bypass -File scripts\agent.ps1 -Token <secret>
# It listens on TCP -Port (allow it in the firewall, private networks only)
# for one JSON request per connection: {"token":..., "op":..., ...}, and
# answers one JSON line. Ops:
#   ping                          host name, game instances running
#   start  count                  starts game instances (scripts\up.ps1)
#   stop                          closes them
#   cmd    instance commands      mod commands (scripts\cmd.ps1), ';'-separated
#   log    instance from          the instance's log from line `from` on
#   put    path data              writes a file (base64) under the game folder
#                                 (Binaries\Win64\...) or this folder (sfv-rollback\...)
#   get    path                   reads such a file (base64)
#   ini    file section key value sets a key in an ini under Binaries\Win64
#   shot   instance               a screenshot (PNG, base64)
#   server port                   starts the Weyvelength server here (scripts\weyve-server.ps1)
# Anyone who can reach the port and knows the token can run game commands
# and write files in the game folder: LAN only, a token you made up.
param([Parameter(Mandatory)][string]$Token, [int]$Port = 47100)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$game = (Resolve-Path (Join-Path $PSScriptRoot '..\..\StreetFighterV\Binaries\Win64')).Path
function LogOf([int]$i) { Join-Path $game ($(if ($i -eq 0) { 'SFVR\logs\sfvr.log' } else { "SFVR\logs\sfvr.$i.log" })) }
. (Join-Path $PSScriptRoot 'logtail.ps1')
$script:logState = @{} # the log op's per instance tails
# A path the request names, kept inside the game's Win64 folder or the repo.
function Inside([string]$path) {
  $base = if ($path -like 'sfv-rollback\*' -or $path -like 'sfv-rollback/*') { Split-Path $repo -Parent } else { $game }
  $full = [IO.Path]::GetFullPath((Join-Path $base $path))
  if (-not ($full.StartsWith($game, 'OrdinalIgnoreCase') -or $full.StartsWith($repo, 'OrdinalIgnoreCase'))) { throw "outside the game folder: $path" }
  $full
}

function Handle($q) {
  switch ($q.op) {
    'ping' {
      return @{ host = $env:COMPUTERNAME; games = @(Get-Process StreetFighterV -ErrorAction SilentlyContinue).Count; game = $game }
    }
    'start' {
      $out = & (Join-Path $PSScriptRoot 'up.ps1') -Count ([int]$q.count) 2>&1 | Out-String
      return @{ output = $out }
    }
    'stop' {
      Get-Process StreetFighterV -ErrorAction SilentlyContinue | Stop-Process -Force
      return @{ output = 'stopped' }
    }
    'cmd' {
      $out = & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance ([int]$q.instance) -Commands $q.commands -WaitMs 0 2>&1 | Out-String
      return @{ output = $out }
    }
    'log' {
      # At most 3000 lines per call; from -1: only the line count (to start
      # reading from now on). A caller that goes on from where the last call
      # ended gets only what was added since (logtail.ps1): reading the whole
      # log on each call (every second in a test) took CPU from the game.
      # Per instance: a tail of the log holding the lines not returned yet,
      # and how many were (the next `from`).
      $i = [int]$q.instance
      $from = [int]$q.from
      $s = $script:logState[$i]
      if (-not $s -or $from -lt 0 -or $from -ne $s.returned) {
        # Number the lines from the log's start again.
        $t = New-LogTail (LogOf $i)
        $t.Offset = 0
        Update-LogTail $t
        $s = @{ tail = $t; returned = 0 }
        $script:logState[$i] = $s
        $skip = if ($from -lt 0) { $t.Lines.Count } else { [Math]::Min($from, $t.Lines.Count) }
        $t.Lines.RemoveRange(0, $skip)
        $s.returned = $skip
        if ($from -lt 0) { return @{ total = $s.returned; lines = @() } }
      }
      else { Update-LogTail $s.tail }
      $t = $s.tail
      $n = [Math]::Min(3000, $t.Lines.Count)
      $lines = if ($n -gt 0) { $t.Lines.GetRange(0, $n).ToArray() } else { @() }
      $t.Lines.RemoveRange(0, $n)
      $s.returned += $n
      return @{ total = $s.returned; lines = @($lines) }
    }
    'put' {
      $full = Inside $q.path
      New-Item -ItemType Directory -Force -Path (Split-Path $full -Parent) | Out-Null
      [IO.File]::WriteAllBytes($full, [Convert]::FromBase64String($q.data))
      return @{ output = "wrote $full" }
    }
    'get' {
      $full = Inside $q.path
      return @{ data = [Convert]::ToBase64String([IO.File]::ReadAllBytes($full)) }
    }
    'ini' {
      $full = Inside $q.file
      Add-Type -Namespace Native -Name Ini -MemberDefinition '[DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern bool WritePrivateProfileString(string s, string k, string v, string f);' -ErrorAction SilentlyContinue
      [Native.Ini]::WritePrivateProfileString($q.section, $q.key, $q.value, $full) | Out-Null
      return @{ output = "set [$($q.section)] $($q.key)=$($q.value) in $full" }
    }
    'shot' {
      $png = Join-Path $env:TEMP "sfv-agent-shot.png"
      & (Join-Path $PSScriptRoot 'win.ps1') -Action shot -Instance ([int]$q.instance) -Out $png | Out-Null
      return @{ data = [Convert]::ToBase64String([IO.File]::ReadAllBytes($png)) }
    }
    'server' {
      $out = & (Join-Path $PSScriptRoot 'weyve-server.ps1') -Port ([int]$q.port) 2>&1 | Out-String
      return @{ output = $out }
    }
    default { throw "unknown op '$($q.op)'" }
  }
}

$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Any, $Port)
$listener.Start()
"agent on port $Port (game folder $game); Ctrl+C stops it"
while ($true) {
  $client = $listener.AcceptTcpClient()
  try {
    $client.ReceiveTimeout = 30000
    $stream = $client.GetStream()
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
    $q = $reader.ReadLine() | ConvertFrom-Json
    if ($q.token -ne $Token) { throw 'bad token' }
    $answer = @{ ok = $true } + (Handle $q)
    "$(Get-Date -Format HH:mm:ss) $($q.op) from $($client.Client.RemoteEndPoint)"
  }
  catch {
    $answer = @{ ok = $false; error = "$_" }
    "$(Get-Date -Format HH:mm:ss) error: $_"
  }
  try {
    $writer.WriteLine(($answer | ConvertTo-Json -Compress -Depth 4))
    $writer.Flush()
  }
  catch { }
  $client.Close()
}
