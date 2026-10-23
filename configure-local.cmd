@echo off
setlocal
cd /d "%~dp0"
for %%D in (downloads binary-cache tmp registries buildtrees packages installed) do if not exist ".deps\%%D" mkdir ".deps\%%D"
set "TEMP=%CD%\.deps\tmp"
set "TMP=%CD%\.deps\tmp"
set "VCPKG_DISABLE_METRICS=1"
set "VCPKG_DOWNLOADS=%CD%\.deps\downloads"
set "VCPKG_DEFAULT_BINARY_CACHE=%CD%\.deps\binary-cache"
set "X_VCPKG_REGISTRIES_CACHE=%CD%\.deps\registries"
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
"C:\Program Files\JetBrains\CLion 2026.2.3\bin\cmake\win\x64\bin\cmake.exe" --preset windows-local
exit /b %ERRORLEVEL%
