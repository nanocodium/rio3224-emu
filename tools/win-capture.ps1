# win-capture.ps1 [-Process NAME] [-OutDir DIR] : screenshot every top-level window of a process (PrintWindow with
# PW_RENDERFULLCONTENT, so windows hidden behind others are captured too) -> DIR\NAME-<pid>-<n>.png
param([string]$Process = "DanteController", [string]$OutDir = "D:\rio3224-emu\out")
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public class WinCap {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out RECT r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int cmd);
  [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr hwnd);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr hwnd, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
  public delegate bool EnumProc(IntPtr hwnd, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static List<IntPtr> Windows(uint pid) { var list = new List<IntPtr>(); EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid && IsWindowVisible(h) && GetWindowTextLength(h) > 0) list.Add(h); return true; }, IntPtr.Zero); return list; }
  public static string Title(IntPtr h) { var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString(); }
}
"@
$n = 0
foreach ($p in Get-Process -Name $Process -ErrorAction SilentlyContinue) {
  foreach ($h in [WinCap]::Windows($p.Id)) {
    if ([WinCap]::IsIconic($h)) { [void][WinCap]::ShowWindow($h, 9); Start-Sleep -Milliseconds 700 }   # restore a minimised window so it can be drawn
    $r = New-Object WinCap+RECT; [void][WinCap]::GetWindowRect($h, [ref]$r)
    $w = $r.R - $r.L; $hgt = $r.B - $r.T; if ($w -lt 50 -or $hgt -lt 50) { continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    $ok = [WinCap]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc); $g.Dispose()
    $file = Join-Path $OutDir ("{0}-{1}-{2}.png" -f $Process, $p.Id, $n); $bmp.Save($file); $bmp.Dispose(); $n++
    "{0} | pid {1} | {2}x{3} | '{4}' | ok={5}" -f $file, $p.Id, $w, $hgt, [WinCap]::Title($h), $ok
  }
}
if ($n -eq 0) { "no visible windows for process '$Process'" }
