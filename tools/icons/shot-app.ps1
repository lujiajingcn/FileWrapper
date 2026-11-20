# 鍚姩 FileWrapper 骞舵埅鍥剧獥鍙ｏ紙浠呯敤浜庢湰鍦拌瑙夋牳瀵癸紝涓嶅弬涓庝骇鍝佹瀯寤猴級銆?# 鐢ㄦ硶锛?#   powershell -File tools/icons/shot-app.ps1 [-Out shot.png] [-OpenFile archive.fwda] [-OpenMenu] [-Width 1100] [-Height 760]
param(
    [string]$Out = 'ui-review/shots/icons-main.png',
    [string]$OpenFile = '',
    [switch]$OpenMenu,
    [switch]$OpenMerge,
    [int]$Width = 1100,
    [int]$Height = 760
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$exe = Join-Path $root 'build/Desktop_Qt_6_9_3_MinGW_64_bit_Debug/FileWrapper/debug/FileWrapper.exe'
$qtBin = 'E:\V_C\Qt\6.9.3\mingw_64\bin'
if (-not (Test-Path $exe)) { throw "鎵句笉鍒?$exe" }

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class FwWin {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, IntPtr extra);
}
"@
Add-Type -AssemblyName System.Drawing

$env:PATH = "$qtBin;$env:PATH"
$proc = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -PassThru
$wsh = New-Object -ComObject WScript.Shell
try {
    for ($i = 0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 500
        $proc.Refresh()
        if ($proc.HasExited) { throw "绋嬪簭宸查€€鍑猴紝exit=$($proc.ExitCode)" }
        if ($proc.MainWindowHandle -ne 0) { break }
    }
    if ($proc.MainWindowHandle -eq 0) { throw '绐楀彛娌℃湁鍑虹幇' }
    Start-Sleep -Seconds 2

    $h = $proc.MainWindowHandle
    $TOP = [IntPtr](-1)
    $NOTOP = [IntPtr](-2)
    [FwWin]::SetWindowPos($h, $TOP, 20, 20, $Width, $Height, 0x0040) | Out-Null
    [FwWin]::ShowWindow($h, 9) | Out-Null
    [FwWin]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 800

    if ($OpenFile) {
        [void]$wsh.AppActivate($proc.Id)
        Start-Sleep -Milliseconds 300
        $wsh.SendKeys('^o')
        Start-Sleep -Seconds 2
        # 鐢ㄥ壀璐存澘绮樿创鏁存潯璺緞锛氶€愬瓧绗?SendKeys 浼氳鍘熺敓瀵硅瘽妗嗙殑鑷姩琛ュ叏鍚炲瓧
        Set-Clipboard -Value $OpenFile
        Start-Sleep -Milliseconds 400
        $wsh.SendKeys('^v')
        Start-Sleep -Milliseconds 800
        $wsh.SendKeys('{ENTER}')
        Start-Sleep -Seconds 4
    }

    if ($OpenMerge) {
        [void]$wsh.AppActivate($proc.Id)
        Start-Sleep -Milliseconds 300
        $wsh.SendKeys('^m')
        Start-Sleep -Seconds 2
    }

    if ($OpenMenu) {
        $r0 = New-Object FwWin+RECT
        [FwWin]::GetWindowRect($h, [ref]$r0) | Out-Null
        [FwWin]::SetCursorPos($r0.Left + 38, $r0.Top + 45) | Out-Null
        Start-Sleep -Milliseconds 300
        [FwWin]::mouse_event(0x0002, 0, 0, 0, [IntPtr]::Zero)   # LEFTDOWN
        [FwWin]::mouse_event(0x0004, 0, 0, 0, [IntPtr]::Zero)   # LEFTUP
        Start-Sleep -Seconds 1
    }

    $r = New-Object FwWin+RECT
    [FwWin]::GetWindowRect($h, [ref]$r) | Out-Null
    $w = $r.Right - $r.Left; $hgt = $r.Bottom - $r.Top
    $bmp = New-Object System.Drawing.Bitmap $w, $hgt
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.Left, $r.Top, 0, 0, (New-Object System.Drawing.Size $w, $hgt))
    $g.Dispose()

    $target = Join-Path $root $Out
    New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
    $bmp.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    [FwWin]::SetWindowPos($h, $NOTOP, 0, 0, 0, 0, 0x0001 -bor 0x0002) | Out-Null
    "saved $target ($w x $hgt)"
}
finally {
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
}



