@echo off
setlocal
cd /d "%~dp0"
set "TEMP=%CD%\.deps\tmp"
set "TMP=%CD%\.deps\tmp"
set "PATH=%CD%\.deps\installed-core\x64-windows\debug\bin;%PATH%"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cmake --build out/build/windows-core
exit /b %ERRORLEVEL%
