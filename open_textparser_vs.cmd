@echo off

set VCPKG_ROOT=C:\vcpkg
set VCPKG_DEFAULT_TRIPLET=x64-windows
set VCPKG_FEATURE_FLAGS=manifests

for /f "usebackq delims=" %%i in (`
  "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" ^
  -latest -products * -requires Microsoft.Component.MSBuild ^
  -find **\Common7\IDE\devenv.exe
`) do (
  set DEVENV=%%i
)

if not defined DEVENV (
  echo Visual Studio not found.
  pause
  exit /b 1
)

start "" "%DEVENV%" TextParser.sln