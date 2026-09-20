$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class Win32 {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, IntPtr pid);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool attach);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessageW(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    public static IntPtr _hwnd;
    public static void Focus(IntPtr hwnd) {
        ShowWindow(hwnd, 9);
        IntPtr fg = GetForegroundWindow();
        uint fgTid = GetWindowThreadProcessId(fg, IntPtr.Zero);
        uint myTid = GetCurrentThreadId();
        AttachThreadInput(myTid, fgTid, true);
        SetForegroundWindow(hwnd);
        AttachThreadInput(myTid, fgTid, false);
    }
    public static void TapVK(int vk, ushort scan) {
        // WM_KEYDOWN / WM_KEYUP posted to the window: works even when the
        // foreground lock denies a real focus switch.
        IntPtr down = (IntPtr)(0x00000001 | (scan << 16));
        IntPtr up = (IntPtr)(0x00000001 | (scan << 16) | unchecked((int)0xC0000000));
        PostMessageW(_hwnd, 0x0100, (IntPtr)vk, down);
        System.Threading.Thread.Sleep(80);
        PostMessageW(_hwnd, 0x0101, (IntPtr)vk, up);
    }
}
"@
$proc = Get-Process eden -ErrorAction Stop
$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) { throw "no main window" }
[Win32]::_hwnd = $hwnd

# Best-effort focus (foreground lock may deny while the user is active).
[Win32]::Focus($hwnd)
Start-Sleep -Milliseconds 300
$ok = [Win32]::GetForegroundWindow() -eq $hwnd
Write-Output ("focus_ok=" + $ok)

# X (VK 0x58, scancode 0x2D) is mapped to Switch A by the autotest ini.
[Win32]::TapVK(0x58, 0x2D)
Write-Output "done"
