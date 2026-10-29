# Samples one instance's file reads and CPU (whole process and its busiest
# threads) while it runs mod commands, for example a battle load:
#   load-profile.ps1 -Instance 1 -Commands 'setup ...; battlenow' -Seconds 25
# Prints one line per interval: MB read, CPU seconds used (all threads), and
# the threads that used the most CPU in it.
param([int]$Instance = 0, [string]$Commands = '', [int]$Seconds = 25, [int]$IntervalMs = 500, [int]$Top = 4)
$title = "StreetFighterV [sfv $Instance]"
$p = Get-Process StreetFighterV | Where-Object { $_.MainWindowTitle -eq $title } | Select-Object -First 1
if (-not $p) { "no window '$title'"; exit 1 }
function Sample {
  $p.Refresh()
  $io = (Get-CimInstance Win32_Process -Filter "ProcessId=$($p.Id)").ReadTransferCount
  $threads = @{}
  foreach ($t in $p.Threads) { try { $threads[$t.Id] = $t.TotalProcessorTime.TotalSeconds } catch {} }
  [pscustomobject]@{ At = Get-Date; Read = $io; Cpu = $p.TotalProcessorTime.TotalSeconds; Threads = $threads }
}
$last = Sample
if ($Commands) { & (Join-Path $PSScriptRoot 'cmd.ps1') -Instance $Instance -Commands $Commands -WaitMs 0 | Out-Null }
$start = $last.At
$totalRead = 0; $totalCpu = 0
while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
  Start-Sleep -Milliseconds $IntervalMs
  $now = Sample
  $dt = ($now.At - $last.At).TotalSeconds
  $mb = ($now.Read - $last.Read) / 1MB
  $cpu = $now.Cpu - $last.Cpu
  $totalRead += $mb; $totalCpu += $cpu
  $busy = $now.Threads.Keys | Where-Object { $last.Threads.ContainsKey($_) } |
    ForEach-Object { [pscustomobject]@{ Id = $_; S = $now.Threads[$_] - $last.Threads[$_] } } |
    Sort-Object S -Descending | Select-Object -First $Top |
    ForEach-Object { '{0}:{1:P0}' -f $_.Id, ($_.S / $dt) }
  '{0,6:N1}s  read {1,7:N1} MB  cpu {2,5:N2} cores  {3}' -f ($now.At - $start).TotalSeconds, $mb, ($cpu / $dt), ($busy -join ' ')
  $last = $now
}
'total: read {0:N0} MB, cpu {1:N1} s over {2} s' -f $totalRead, $totalCpu, $Seconds
