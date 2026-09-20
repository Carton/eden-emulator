param([ValidateSet('left','right','none')][string]$Dir = 'right',
      [int]$HoldSec = 20,
      [int]$SettleMs = 800)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Win32R {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr extra; }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public KEYBDINPUT ki; public long pad; }
    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint SendInput(uint n, INPUT[] p, int size);
    public static void KeyScan(ushort vk, bool up) {
        INPUT[] i = new INPUT[1];
        i[0].type = 1; i[0].ki.wVk = vk; i[0].ki.wScan = 0;
        i[0].ki.dwFlags = up ? (uint)0x0002 : (uint)0x0000;
        SendInput(1, i, 40);
    }
    public static void TapScan(ushort scan) {
        KeyScan(scan, false);
        System.Threading.Thread.Sleep(100);
        KeyScan(scan, true);
    }
    public static bool Focus(IntPtr hwnd) {
        ShowWindow(hwnd, 9);
        IntPtr fg = GetForegroundWindow();
        uint fgTid = GetWindowThreadProcessId(fg, IntPtr.Zero);
        uint myTid = GetCurrentThreadId();
        AttachThreadInput(myTid, fgTid, true);
        SetForegroundWindow(hwnd);
        AttachThreadInput(myTid, fgTid, false);
        return GetForegroundWindow() == hwnd;
    }
}
"@
$proc = Get-Process eden -ErrorAction Stop
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw "no main window" }
$ok = [Win32R]::Focus($hwnd)
if (-not $ok) {
    [Win32R]::TapScan(0x38)  # Alt tap grants foreground rights
    Start-Sleep -Milliseconds 200
    $ok = [Win32R]::Focus($hwnd)
}
Write-Output ("focus_ok=" + $ok)
Start-Sleep -Milliseconds $SettleMs
$scan = 0
if ($Dir -eq 'left')  { $scan = 0x4A }
if ($Dir -eq 'right') { $scan = 0x4C }
if ($scan -ne 0) {
    [Win32R]::KeyScan($scan, $false)
    Start-Sleep -Seconds $HoldSec
    [Win32R]::KeyScan($scan, $true)
    Write-Output ("held " + $Dir + " for " + $HoldSec + "s")
} else {
    Start-Sleep -Seconds $HoldSec
    Write-Output "idle window"
}
