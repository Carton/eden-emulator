param(
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$ProcessId,
    [ValidateSet('left','right','none')][string]$Dir = 'right',
    [ValidateRange(1,3600)][int]$HoldSec = 20,
    [ValidateRange(0,10000)][int]$SettleMs = 800
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\window_input.ps1"
$window = Get-EdenWindow $ProcessId
Start-Sleep -Milliseconds $SettleMs
if ($Dir -eq 'none') {
    $mark = [EdenInput]::LastInput()
    Start-Sleep -Seconds $HoldSec
    if ([EdenInput]::LastInput() -ne $mark) { throw 'User input invalidates rotation window' }
} else {
    $vk = if ($Dir -eq 'left') { 0x4A } else { 0x4C }
    $scan = if ($Dir -eq 'left') { 0x24 } else { 0x26 }
    Send-EdenKey $window $vk $scan ($HoldSec * 1000)
}
Write-Output 'input_ok=True'
