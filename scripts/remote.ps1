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

# New log lines of a remote instance since the last call (all on the first).
# -FromNow: skip what is there (the next call returns only newer lines).
function RemoteLog([int]$instance = 0, [switch]$FromNow) {
  if ($FromNow) {
    $script:remoteLogLines[$instance] = [int](Remote 'log' @{ instance = $instance; from = -1 }).total
    return
  }
  $out = @()
  do {
    $from = if ($script:remoteLogLines.ContainsKey($instance)) { $script:remoteLogLines[$instance] } else { 0 }
    $a = Remote 'log' @{ instance = $instance; from = $from }
    $script:remoteLogLines[$instance] = [int]$a.total
    $out += @($a.lines)
  } while (@($a.lines).Count -ge 3000)
  $out
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
