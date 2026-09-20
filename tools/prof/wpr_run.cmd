@echo off
setlocal
if not defined EDEN_PYTHON set "EDEN_PYTHON=python"
"%EDEN_PYTHON%" "%~dp0wpr_capture.py" %*
exit /b %errorlevel%
