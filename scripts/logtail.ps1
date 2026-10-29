# Follows a game log from now on, reading only what was added since the last
# call (dot-source it). Reading the whole log every second (Get-Content of a
# few MB) cost about 0.8 s of CPU each time and the game on the same PC lost
# frames to it.
#   $t = New-LogTail $path      # from the current end
#   Update-LogTail $t           # adds the new complete lines to $t.Lines
#   $t.Lines                    # everything since New-LogTail
function New-LogTail([string]$Path) {
  $offset = if (Test-Path $Path) { (Get-Item $Path).Length } else { 0 }
  [pscustomobject]@{ Path = $Path; Offset = [long]$offset; Lines = [System.Collections.Generic.List[string]]::new() }
}

function Update-LogTail($Tail) {
  if (-not (Test-Path $Tail.Path)) { return }
  # Shared read: the game has the log open for writing.
  $fs = [IO.FileStream]::new($Tail.Path, 'Open', 'Read', 'ReadWrite')
  try {
    if ($fs.Length -lt $Tail.Offset) { $Tail.Offset = 0 } # a new log
    $count = $fs.Length - $Tail.Offset
    if ($count -le 0) { return }
    $bytes = [byte[]]::new($count)
    [void]$fs.Seek($Tail.Offset, 'Begin')
    $read = 0
    while ($read -lt $count) { $n = $fs.Read($bytes, $read, $count - $read); if ($n -le 0) { break }; $read += $n }
  }
  finally { $fs.Close() }
  if ($read -le 0) { return }
  # Complete lines only; a line still being written waits for the next call.
  $end = [Array]::LastIndexOf($bytes, [byte]10, $read - 1)
  if ($end -lt 0) { return }
  $text = [Text.Encoding]::UTF8.GetString($bytes, 0, $end)
  foreach ($line in $text -split "`r?`n") { $Tail.Lines.Add($line) }
  $Tail.Offset += $end + 1
}
