@echo off
setlocal

echo =====================================
echo   CXOS Lite - CLEAN ^& REGENERATE BUILD
echo =====================================

cd /d %~dp0..
set ROOT=%cd%

REM ---- wipe the build folder (all generated artifacts) ----
if exist build (
    echo Removing old build folder...
    rmdir /s /q build
)

mkdir build
cd build

REM ---- regenerate with CMake ----
echo Regenerating CMake project...
cmake .. -G "NMake Makefiles"
if errorlevel 1 (
    echo [ERROR] CMake configuration failed.
    exit /b 1
)

REM ---- build ----
echo Building...
cmake --build .
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo =====================================
echo   CLEAN BUILD COMPLETE
echo =====================================

endlocal