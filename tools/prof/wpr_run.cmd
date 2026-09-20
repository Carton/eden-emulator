@echo off
wpr -cancel >nul 2>&1
wpr -start CPU -start GPU -filemode -recordtempto F:\prof > F:\prof\wpr_run.log 2>&1
echo STARTED >> F:\prof\wpr_run.log
wpr -status >> F:\prof\wpr_run.log 2>&1
timeout /t 100 /nobreak >nul
echo PRE-STOP-STATUS >> F:\prof\wpr_run.log
wpr -status >> F:\prof\wpr_run.log 2>&1
wpr -stop F:\prof\totk_t3.etl >> F:\prof\wpr_run.log 2>&1
echo WPR_DONE >> F:\prof\wpr_run.log
