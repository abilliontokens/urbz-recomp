param([string]$Process = "Urbz", [string]$Out = "D:\Urbz\out\shot.png", [int]$Width = 960)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Win {
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT p);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder s, int n);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

$p = Get-Process $Process -ErrorAction Stop
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Output "no main window"; exit 1 }
[void][Win]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 800

# whose window is actually on top when the shot is taken
$fg = [Win]::GetForegroundWindow()
$sb = New-Object System.Text.StringBuilder 256
[void][Win]::GetWindowTextW($fg, $sb, 256)
$same = ($fg -eq $h)
Write-Output ("foreground '{0}' isGame={1}" -f $sb.ToString(), $same)

# client area only: no title bar, no desktop around it
$r = New-Object Win+RECT
[void][Win]::GetClientRect($h, [ref]$r)
$origin = New-Object Win+POINT
[void][Win]::ClientToScreen($h, [ref]$origin)
$w = $r.Right - $r.Left
$ht = $r.Bottom - $r.Top
Write-Output ("client {0}x{1} at {2},{3}" -f $w, $ht, $origin.X, $origin.Y)

$bmp = New-Object System.Drawing.Bitmap $w, $ht
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($origin.X, $origin.Y, 0, 0, (New-Object System.Drawing.Size $w, $ht))
$g.Dispose()

$scale = $Width / $w
$small = New-Object System.Drawing.Bitmap $Width, ([int]($ht * $scale))
$g2 = [System.Drawing.Graphics]::FromImage($small)
$g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$g2.DrawImage($bmp, 0, 0, $small.Width, $small.Height)
$g2.Dispose()
$small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()

$lit = 0; $total = 0; $sum = 0
for ($y = 0; $y -lt $small.Height; $y += 4) {
    for ($x = 0; $x -lt $small.Width; $x += 4) {
        $c = $small.GetPixel($x, $y)
        $total++
        $v = $c.R + $c.G + $c.B
        $sum += $v
        if ($v -gt 24) { $lit++ }
    }
}
$small.Dispose()
Write-Output ("saved {0}: {1:N1}% lit, mean {2:N0}/765" -f $Out, (100.0 * $lit / $total), ($sum / $total))
