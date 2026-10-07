# PID-targeted window messages avoid injecting keys into an unrelated foreground app.
if (-not ('EdenInput' -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class EdenInput {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUTINFO { public uint size; public uint tick; }
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessageW(IntPtr hwnd, uint msg, IntPtr wparam, IntPtr lparam);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool GetLastInputInfo(ref INPUTINFO info);
    public delegate bool EnumProc(IntPtr hwnd, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr param);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hwnd, System.Text.StringBuilder text, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    public static IntPtr RenderWindow(int pid) {
        IntPtr found = IntPtr.Zero;
        EnumProc child = (hwnd, param) => {
            uint owner; GetWindowThreadProcessId(hwnd, out owner);
            var title = new System.Text.StringBuilder(256);
            GetWindowTextW(hwnd, title, title.Capacity);
            if (owner == pid && IsWindowVisible(hwnd) && title.ToString() == "Form") found = hwnd;
            return true;
        };
        EnumWindows((hwnd, param) => { child(hwnd, param); EnumChildWindows(hwnd, child, param); return true; }, IntPtr.Zero);
        return found;
    }
    public static uint LastInput() {
        INPUTINFO info = new INPUTINFO();
        info.size = (uint)Marshal.SizeOf(typeof(INPUTINFO));
        if (!GetLastInputInfo(ref info)) throw new System.ComponentModel.Win32Exception();
        return info.tick;
    }
}
"@
}

function Get-EdenWindow([int]$ProcessId) {
    $process = Get-Process -Id $ProcessId -ErrorAction Stop
    if ($process.ProcessName -ne 'eden') { throw 'PID is not eden.exe' }
    # GRenderWindow handles Qt key events; the main window does not.
    $window = [EdenInput]::RenderWindow($ProcessId)
    if ($window -eq [IntPtr]::Zero) { throw 'No visible Form render window for this PID' }
    return $window
}

function Send-EdenKey([IntPtr]$Window, [int]$Vk, [int]$Scan, [int]$Milliseconds) {
    $mark = [EdenInput]::LastInput()
    $down = [IntPtr](1 -bor ($Scan -shl 16))
    $up = [IntPtr](1 -bor ($Scan -shl 16) -bor -1073741824)
    try {
        if (-not [EdenInput]::PostMessageW($Window, 0x100, [IntPtr]$Vk, $down)) {
            throw 'WM_KEYDOWN failed'
        }
        Start-Sleep -Milliseconds $Milliseconds
    } finally {
        if (-not [EdenInput]::PostMessageW($Window, 0x101, [IntPtr]$Vk, $up)) {
            throw 'WM_KEYUP failed'
        }
    }
    # EDEN_IGNORE_INPUT: skip the global-input check (phantom-input devices);
    # same trade-off as eden_session.input_tick - detection is fully disabled.
    if (-not $env:EDEN_IGNORE_INPUT -and [EdenInput]::LastInput() -ne $mark) {
        throw 'User input invalidates test window'
    }
}
