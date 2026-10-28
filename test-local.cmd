@echo off
setlocal
cd /d "%~dp0"
set "TEMP=%CD%\.deps\tmp"
set "TMP=%CD%\.deps\tmp"
set "PATH=%CD%\.deps\installed-core\x64-windows\debug\bin;%PATH%"
ctest --test-dir out/build/windows-core --output-on-failure --no-tests=error %*
exit /b %ERRORLEVEL%
