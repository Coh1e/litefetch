@echo off
setlocal
cd /d "%~dp0"
set "TEMP=%CD%\.deps\tmp"
set "TMP=%CD%\.deps\tmp"
set "PATH=%CD%\.deps\installed-core\x64-windows\debug\bin;%CD%\.deps\installed\x64-windows\debug\bin;%PATH%"
if "%~1"=="--torrent-selection" goto torrent_selection
if "%~1"=="--amule-runtime" goto amule_runtime
"C:\Program Files\JetBrains\CLion 2026.2.3\bin\cmake\win\x64\bin\ctest.exe" --test-dir out/build/windows-core --output-on-failure --no-tests=error %*
exit /b %ERRORLEVEL%
:torrent_selection
out\build\windows-local\download-torrent-tests.exe --only-selection
exit /b %ERRORLEVEL%
:amule_runtime
out\build\windows-local\download-amule-runtime-tests.exe "%CD%\tools\amule-3.1.0\amule-portable-x64\bin" "%CD%\out\verification"
exit /b %ERRORLEVEL%
