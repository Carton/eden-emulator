@echo off
setlocal
REM Run from an elevated terminal. No implicit UAC relaunch loses CLI arguments.
if not defined EDEN_PYTHON set "EDEN_PYTHON=python"
"%EDEN_PYTHON%" "%~dp0stutter_watch.py" %*
exit /b %errorlevel%
