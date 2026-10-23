@echo off
setlocal
cd /d "%~dp0"
set "TEMP=%CD%\.deps\tmp"
set "TMP=%CD%\.deps\tmp"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
"C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\verify-downloader-dependencies.ps1"
if errorlevel 1 exit /b 1
set "LOCAL_CMAKE=C:\Program Files\JetBrains\CLion 2026.2.3\bin\cmake\win\x64\bin\cmake.exe"
"%LOCAL_CMAKE%" --build --preset windows-local
if errorlevel 1 exit /b 1
"%LOCAL_CMAKE%" --install out/build/windows-local --config Debug
exit /b %ERRORLEVEL%
