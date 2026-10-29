param([string]$Action='shot', [string]$Keys='', [int]$HoldMs=80, [int]$GapMs=250, [string]$Out='', [int]$Instance=0,
      [int]$X=0, [int]$Y=0)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
}
"@
$all = Get-Process StreetFighterV -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
$p = $all | Where-Object { $_.MainWindowTitle -like "*[[]sfv $Instance[]]*" } | Select-Object -First 1
if (-not $p -and $Instance -eq 0) { $p = $all | Where-Object { $_.MainWindowTitle -notlike "*[[]sfv *" } | Select-Object -First 1 }
if (-not $p) { "no game window"; exit 1 }
$h = $p.MainWindowHandle
function Focus { if ([W]::IsIconic($h)) { [W]::ShowWindow($h, 9) | Out-Null; Start-Sleep -Milliseconds 500 }; if ([W]::GetForegroundWindow() -ne $h) { [W]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [W]::keybd_event(0x12,0,2,[UIntPtr]::Zero); [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 300 } }
$vk = @{ f1=0x70; up=0x26; down=0x28; left=0x25; right=0x27; enter=0x0D; esc=0x1B; space=0x20; f5=0x74; f6=0x75; f9=0x78; f10=0x79; f11=0x7A; f12=0x7B; tab=0x09; back=0x08 }
if ($Action -eq 'keys') {
  Focus
  foreach ($k in $Keys.Split(' ', [StringSplitOptions]::RemoveEmptyEntries)) {
    if ($k -match '^wait(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
    $code = if ($vk.ContainsKey($k)) { $vk[$k] } else { [int][char]$k.ToUpper() }
    $scan = [W]::MapVirtualKey($code, 0); $ext = if ($code -in 0x25..0x28) { 1 } else { 0 }
    [W]::keybd_event([byte]$code, [byte]$scan, [uint32]$ext, [UIntPtr]::Zero); Start-Sleep -Milliseconds $HoldMs
    [W]::keybd_event([byte]$code, [byte]$scan, [uint32](2 -bor $ext), [UIntPtr]::Zero); Start-Sleep -Milliseconds $GapMs
  }
  "sent"
}
if ($Action -eq 'click') {
  # -X -Y in screenshot pixels (half the window size, title bar included):
  # posted as a left click to the window's client area (no focus needed).
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $o = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$o) | Out-Null
  $cx = $X * 2 - ($o.X - $r.L); $cy = $Y * 2 - ($o.Y - $r.T)
  $lp = [IntPtr](($cy -shl 16) -bor ($cx -band 0xFFFF))
  [W]::PostMessage($h, 0x0200, [IntPtr]::Zero, $lp) | Out-Null; Start-Sleep -Milliseconds 120
  [W]::PostMessage($h, 0x0201, [IntPtr]1, $lp) | Out-Null; Start-Sleep -Milliseconds 120
  [W]::PostMessage($h, 0x0202, [IntPtr]::Zero, $lp) | Out-Null; Start-Sleep -Milliseconds 300
  "clicked $cx,$cy"
}
if ($Action -eq 'shot' -or $Action -eq 'keys' -or $Action -eq 'click') {
  if ($Out -eq '') { exit 0 }
  if ([W]::IsIconic($h)) { [W]::ShowWindow($h, 4) | Out-Null; Start-Sleep -Milliseconds 400 }
  $r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
  $wd = $r.R - $r.L; $ht = $r.B - $r.T
  $bmp = New-Object System.Drawing.Bitmap $wd, $ht
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [W]::PrintWindow($h, $hdc, 2) | Out-Null; $g.ReleaseHdc($hdc)
  $small = New-Object System.Drawing.Bitmap $bmp, ([int]($wd/2)), ([int]($ht/2))
  $small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
  "shot $wd x $ht -> $Out"
}
