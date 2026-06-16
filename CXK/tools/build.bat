@echo off
setlocal

echo =====================================
echo   CXOS Lite - BUILD START
echo =====================================

cd /d %~dp0..
if not exist build mkdir build

cd build

echo [1/3] Configuring CMake...
cmake ..

if errorlevel 1 (
    echo ERROR: CMake configuration failed
    exit /b 1
)

echo [2/3] Building project...
cmake --build .

if errorlevel 1 (
    echo ERROR: Build failed
    exit /b 1
)

echo [3/3] Build complete
echo =====================================
echo Output is in /build
echo =====================================

endlocal