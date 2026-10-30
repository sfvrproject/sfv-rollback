# Talks to scripts\agent.ps1 on the second PC. Dot-source it, or run one op:
#   . scripts\remote.ps1 -Agent 192.168.1.20 -Token <secret>
#   Remote ping
#   Remote cmd @{ instance = 0; commands = 'mash 22; join ABCDEF' }
#   RemoteLog 0          # the remote instance's log lines since the last call
#   RemotePut <local file> <remote path>   RemoteGet <remote path> <local file>
#   scripts\remote.ps1 -Agent 192.168.1.20 -Token <secret> -Op ping
param([string]$Agent = $env:SFV_AGENT, [string]$Token = $env:SFV_AGENT_TOKEN, [int]$Port = 47100, [string]$Op = '')

$script:agentHost = $Agent
$script:agentToken = $Token
$script:agentPort = $Port
$script:remoteLogLines = @{}

function Remote([string]$op, [hashtable]$fields = @{}) {
  $q = @{ token = $script:agentToken; op = $op } + $fields
  $client = [Net.Sockets.TcpClient]::new()
  $client.ReceiveTimeout = 180000
  $client.Connect($script:agentHost, $script:agentPort)
  try {
    $stream = $client.GetStream()
    $writer = [IO.StreamWriter]::new($stream, [Text.UTF8Encoding]::new($false))
    $writer.WriteLine(($q | ConvertTo-Json -Compress -Depth 4))
    $writer.Flush()
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8)
    $answer = $reader.ReadLine() | ConvertFrom-Json
  }
  finally { $client.Close() }
  if (-not $answer.ok) { throw "agent: $($answer.error)" }
  $answer
}

$script:remotePending = @{} # lines RemoteCmd -Wait read, kept for the next RemoteLog

function RemoteFetch([int]$instance) {
  $out = @()
  do {
    $from = if ($script:remoteLogLines.ContainsKey($instance)) { $script:remoteLogLines[$instance] } else { 0 }
    $a = Remote 'log' @{ instance = $instance; from = $from }
    $script:remoteLogLines[$instance] = [int]$a.total
    $out += @($a.lines)
  } while (@($a.lines).Count -ge 3000)
  $out
}

# New log lines of a remote instance since the last call (all on the first).
# -FromNow: skip what is there (the next call returns only newer lines).
function RemoteLog([int]$instance = 0, [switch]$FromNow) {
  if ($FromNow) {
    $script:remoteLogLines[$instance] = [int](Remote 'log' @{ instance = $instance; from = -1 }).total
    $script:remotePending[$instance] = @()
    return
  }
  $out = @($script:remotePending[$instance]) + @(RemoteFetch $instance)
  $script:remotePending[$instance] = @()
  $out | Where-Object { $null -ne $_ }
}

# Mod commands on a remote instance; throws when its cmd.ps1 failed (not
# taken, no game running). -Wait: until they have run (an echo marker in its
# log; the lines read meanwhile still come from the next RemoteLog).
function RemoteCmd([int]$instance, [string]$commands, [switch]$Wait, [int]$Seconds = 30) {
  $marker = ''
  if ($Wait) {
    if (-not $script:remoteLogLines.ContainsKey($instance)) { RemoteLog $instance -FromNow }
    $marker = 'done-' + [guid]::NewGuid().ToString('N').Substring(0, 10)
    $commands = "$commands; echo $marker"
  }
  $a = Remote 'cmd' @{ instance = $instance; commands = $commands }
  if ($a.output -match 'did not take|has not taken|no game is running|did not finish') {
    throw "second PC, instance ${instance}: '$commands' failed: $($a.output.Trim())"
  }
  if (-not $Wait) { return }
  $deadline = (Get-Date).AddSeconds($Seconds)
  if (-not $script:remotePending.ContainsKey($instance)) { $script:remotePending[$instance] = @() }
  while ((Get-Date) -lt $deadline) {
    $new = @(RemoteFetch $instance)
    $script:remotePending[$instance] += $new
    if ($new -match [regex]::Escape("cmd: echo $marker")) { return }
    Start-Sleep -Milliseconds 300
  }
  throw "second PC, instance ${instance}: '$commands' did not finish within $Seconds s"
}

# Starts the remote games (its up.ps1: detached, restarted on a crash at
# startup) and throws when they did not reach the main menu.
function RemoteStart([int]$count = 1) {
  $out = (Remote 'start' @{ count = $count }).output
  if ($out -match 'not all started|exited while starting.*retries used|need multi_instance|cannot start') {
    throw "second PC: starting the game failed: $($out.Trim())"
  }
  $out.Trim()
}

# This build's mod and test scripts to the second PC (its up.ps1 copies its
# build folder's DLL into the game folder when it starts the games).
function RemoteDeploy {
  $dll = Join-Path $PSScriptRoot '..\build\Release\sfv_rollback.dll'
  RemotePut $dll 'sfv_rollback.dll' | Out-Null
  RemotePut $dll 'sfv-rollback\build\Release\sfv_rollback.dll' | Out-Null
  foreach ($s in 'cmd.ps1', 'up.ps1', 'lib.ps1', 'logtail.ps1', 'win.ps1', 'weyve-server.ps1') {
    RemotePut (Join-Path $PSScriptRoot $s) "sfv-rollback\scripts\$s" | Out-Null
  }
}

function RemotePut([string]$local, [string]$remotePath) {
  $data = [Convert]::ToBase64String([IO.File]::ReadAllBytes((Resolve-Path $local).Path))
  (Remote 'put' @{ path = $remotePath; data = $data }).output
}

function RemoteGet([string]$remotePath, [string]$local) {
  $a = Remote 'get' @{ path = $remotePath }
  [IO.File]::WriteAllBytes($local, [Convert]::FromBase64String($a.data))
  $local
}

if ($Op) { Remote $Op | ConvertTo-Json -Depth 4 }
