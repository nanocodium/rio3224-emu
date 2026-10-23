# rt-check.ps1 -Args "<rio-emu arguments>" [-Seconds 40] [-Port 8321] [-Tag name]
# Runs build-msvc\rio-emu.exe with the given arguments from build-msvc\, samples the panel's emulated time every
# 5 s against the wall clock and prints the real-time ratio; stderr/stdout go to out\<Tag>.err / .out.
param([string]$EmuArgs, [int]$Seconds = 40, [int]$Port = 8321, [string]$Tag = "rtcheck")
Set-Location D:\rio3224-emu\build-msvc
$p = Start-Process -FilePath ".\rio-emu.exe" -ArgumentList $EmuArgs -RedirectStandardError "..\out\$Tag.err" -RedirectStandardOutput "..\out\$Tag.out" -PassThru
$t0 = Get-Date; $lastEmu = 0; $lastWall = 0
"started pid $($p.Id): $EmuArgs"
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
  Start-Sleep 5
  try {
    $s = (Invoke-WebRequest -UseBasicParsing -TimeoutSec 2 "http://127.0.0.1:$Port/state").Content | ConvertFrom-Json
    $wall = ((Get-Date) - $t0).TotalSeconds * 1000
    $ratio = if ($wall -gt $lastWall) { ($s.ms - $lastEmu) / ($wall - $lastWall) } else { 0 }
    "{0,6:N1} s wall | emulated {1,8} ms | lag {2,7:N0} ms | speed last 5 s {3,5:P0} | clock {4} | fs {5}" -f ($wall/1000), $s.ms, ($wall - $s.ms), $ratio, $s.clockLocked, $s.fs
    $lastEmu = $s.ms; $lastWall = $wall
  } catch { "  (panel not answering yet: $($_.Exception.Message))" }
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep 1
Get-Content "..\out\$Tag.err" | Select-String "stopped:|Minsn|dante-net\] controller" | Select-Object -Last 3
