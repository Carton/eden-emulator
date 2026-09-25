<#
.SYNOPSIS
    以指定 draw-token 实验臂启动 build-vs22 的 eden（评估用，本地工具）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\windows\eden_token.ps1
                  # tail-pipeline 实验臂（Stage 5 后状态）
    powershell -ExecutionPolicy Bypass -File tools\windows\eden_token.ps1 -Mode inline
                  # INLINE token 臂（对照）
    powershell -ExecutionPolicy Bypass -File tools\windows\eden_token.ps1 -Mode serial
                  # 默认串行路径（对照）

.PARAMETER Mode
    tailpipe = EDEN_DRAW_TOKEN=inline + EDEN_TOKEN_TAIL_PIPELINE=1（实验开关）
    inline   = 仅 EDEN_DRAW_TOKEN=inline
    serial   = 不设任何 token 变量（默认路径）

.PARAMETER Check
    附加 EDEN_TOKEN_CHECK=1：影子状态逐 draw 校验，帧率约再降 14%，仅正确性排查用。

.PARAMETER NoGame
    只启动 GUI，不自动加载 TOTK。

.NOTES
    环境变量只影响本脚本启动的进程，不会写进任何配置。
    黄昏水塘参考帧率：tailpipe ~26 / inline ~37 / serial ~42 fps——tailpipe 臂
    比默认慢约 30%，是归档中的实验开关，评估对象是"开关开起来什么样"。
    注意：EDEN_TOKEN_TAIL_PIPELINE=1 单独设置无效，token 路径必须由
    EDEN_DRAW_TOKEN=inline 打开（AGENTS 工作规则第 7 条）。
#>
param(
    [ValidateSet('tailpipe', 'inline', 'serial')]
    [string]$Mode = 'tailpipe',
    [switch]$Check,
    [switch]$NoGame
)

$eden = Join-Path $PSScriptRoot '..\..\build-vs22\bin\eden.exe'
$game = 'F:\prof\TOTK.nsp'

if (-not (Test-Path $eden)) {
    throw "找不到 $eden —— 先按 AGENTS.md 构建 build-vs22。"
}

# 清掉可能残留的 token 变量再按模式设置（避免上一个实验的变量泄漏）。
Remove-Item Env:EDEN_DRAW_TOKEN -ErrorAction SilentlyContinue
Remove-Item Env:EDEN_TOKEN_TAIL_PIPELINE -ErrorAction SilentlyContinue
Remove-Item Env:EDEN_TOKEN_CHECK -ErrorAction SilentlyContinue

switch ($Mode) {
    'tailpipe' {
        $env:EDEN_DRAW_TOKEN = 'inline'
        $env:EDEN_TOKEN_TAIL_PIPELINE = '1'
    }
    'inline' {
        $env:EDEN_DRAW_TOKEN = 'inline'
    }
    'serial' { }
}
if ($Check) {
    $env:EDEN_TOKEN_CHECK = '1'
}

Write-Host ("eden token arm: mode={0} check={1} exe={2}" -f $Mode, [bool]$Check, $eden)

if ($NoGame -or -not (Test-Path $game)) {
    Start-Process -FilePath $eden
} else {
    Start-Process -FilePath $eden -ArgumentList "`"$game`""
}
