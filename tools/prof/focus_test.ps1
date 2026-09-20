param([Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$ProcessId)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\window_input.ps1"
$window = Get-EdenWindow $ProcessId
Send-EdenKey $window 0x58 0x2D 80
Write-Output 'input_ok=True'
