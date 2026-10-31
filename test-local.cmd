@echo off
rem Acceptance: one command, bounded time. The wrapper sets project-local
rem TEMP/TMP and kills the suite if it outlives the cap.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-acceptance.ps1" %*
exit /b %ERRORLEVEL%
