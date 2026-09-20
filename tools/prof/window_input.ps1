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
    $window = $process.MainWindowHandle
    if ($window -eq [IntPtr]::Zero) { throw 'No main window' }
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
    if ([EdenInput]::LastInput() -ne $mark) { throw 'User input invalidates test window' }
}
