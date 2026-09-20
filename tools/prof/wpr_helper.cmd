@echo off
setlocal
if not defined EDEN_PYTHON set "EDEN_PYTHON=python"
if "%~1"=="" (
  echo Usage: wpr_helper.cmd SESSION_DIRECTORY
  exit /b 2
)
"%EDEN_PYTHON%" "%~dp0wpr_capture.py" "%~1" --serve
exit /b %errorlevel%
