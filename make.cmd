@echo off
setlocal EnableExtensions
pushd "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo Error: CMake was not found in PATH.
    echo Run this script from a Visual Studio Developer Command Prompt.
    popd
    exit /b 1
)

if "%~1"=="" goto help
set "task=%~1"
goto dispatch

:dispatch
if /i "%task%"=="configure" goto configure
if /i "%task%"=="build" goto build
if /i "%task%"=="debug" goto debug
if /i "%task%"=="clean" goto clean
if /i "%task%"=="help" goto help
if /i "%task%"=="--help" goto help
if /i "%task%"=="-h" goto help
echo Unknown task: %task%
set "help_exit=2"
goto help

:configure
call :do_configure || goto failure
goto success

:build
call :do_configure || goto failure
call :do_build Release || goto failure
goto success

:debug
call :do_configure || goto failure
call :do_build Debug || goto failure
goto success

:clean
echo Removing build artifacts...
cmake -E remove_directory "%CD%\build"
if errorlevel 1 goto failure
echo Runtime logs and the game-directory recovery journal are intentionally preserved.
goto success

:help
echo.
echo Usage: make.cmd [task]
echo.
echo Tasks:
echo   configure  Generate a 64-bit Visual Studio build tree
echo   build      Build the release executable
echo   debug      Build the debug executable
echo   clean      Remove the build directory only
echo   help       Show this help
echo.
if defined help_exit (
    popd
    exit /b %help_exit%
)
goto success

:do_configure
echo Configuring 64-bit Visual Studio build...
cmake -S . -B build -A x64
exit /b %errorlevel%

:do_build
echo Building %~1 configuration...
cmake --build build --config %~1 --parallel
exit /b %errorlevel%

:failure
echo.
echo Task failed with exit code %errorlevel%.
popd
exit /b 1

:success
popd
exit /b 0
