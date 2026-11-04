@echo off
rem Build the recompiled code on Windows with the Visual Studio Build Tools.
rem Usage: build.bat [Release|Debug]
setlocal
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

set VSROOT=%VSROOT%
if "%VSROOT%"=="" set VSROOT=%APPDATA%\dione\bin\build_tools
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
  for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSROOT=%%i
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set PATH=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%

cmake -S "%~dp0." -B "%~dp0build\native-%CONFIG%" -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% || exit /b 1
cmake --build "%~dp0build\native-%CONFIG%" || exit /b 1
