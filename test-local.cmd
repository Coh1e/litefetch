@echo off
rem Acceptance: run every test in the build tree produced by the windows-core
rem preset. Dependencies are built from source by CMake (FetchContent), so there is
rem no package directory to put on PATH any more.
setlocal
cd /d "%~dp0"
rem The presets point TEMP/TMP here; a fresh checkout does not create it, and
rem MSVC reports D8050 when its temp directory is missing.
if not exist ".deps\tmp" mkdir ".deps\tmp"
set "TEMP=%CD%\.deps\tmp"
set "TMP=%CD%\.deps\tmp"
ctest --test-dir out/build/windows-core --output-on-failure --no-tests=error %*
exit /b %ERRORLEVEL%
