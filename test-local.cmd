@echo off
rem Acceptance: run every test in the build tree produced by the windows-core
rem preset. Dependencies are built from source by CMake (FetchContent), so there is
rem no package directory to put on PATH any more.
setlocal
cd /d "%~dp0"
set "TEMP=%CD%\.deps\tmp"
set "TMP=%CD%\.deps\tmp"
ctest --test-dir out/build/windows-core --output-on-failure --no-tests=error %*
exit /b %ERRORLEVEL%
