@echo off
REM ============================================================
REM  Eden stutter watcher launcher (double-click, accept UAC)
REM  Usage: double-click this file -> start the game and play
REM         severe stutters are captured automatically.
REM         When done, focus this console and press Ctrl+C.
REM  Tunables: edit ARGS below (see stutter_watch.py --help)
REM ============================================================
set ARGS=--severe-ms 50 --max-captures 4

net session >nul 2>&1
if %errorlevel% neq 0 (
  powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
  exit /b
)
cd /d F:\prof
title Eden stutter watcher
python F:\prof\stutter_watch.py %ARGS%
echo.
echo ==== watcher exited, press any key to close ====
pause >nul
