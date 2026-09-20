@echo off
:loop
if exist F:\prof\wpr_go_tailimm_off.cmd (call F:\prof\wpr_go_tailimm_off.cmd & del F:\prof\wpr_go_tailimm_off.cmd >nul 2>&1)
if exist F:\prof\wpr_go_tailimm_on.cmd (call F:\prof\wpr_go_tailimm_on.cmd & del F:\prof\wpr_go_tailimm_on.cmd >nul 2>&1)
if exist F:\prof\wpr_helper_stop (del F:\prof\wpr_helper_stop >nul 2>&1 & exit)
timeout /t 1 /nobreak >nul
goto loop
