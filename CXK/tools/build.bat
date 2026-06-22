@echo off
setlocal
REM /CXK/tools/build.bat
REM Aurora Tejeda / CATX SYSTEMS LLC
REM
REM Clean build of CXK. If the signing key is present, the build SIGNS
REM kernel.xkex + executive.xoex and lays the unified XBPT disk; otherwise it
REM builds unsigned (with a notice). Single entry point - there is no separate
REM "signed" build script.
REM
REM Outputs:
REM   dist\CXK_x86_32\packages\kernel.xkex      (signed if key present)
REM   dist\CXK_x86_32\packages\executive.xoex   (signed if key present)
REM   dist\CXK_x86_32\images\cxk_disk.img        (unified: boot + stage + system)
REM
REM One-time key setup (enables signing):
REM   python tools\CXEX_Compiler\makekeys.py kernel

echo =====================================
echo   CXK - BUILD
echo =====================================

cd /d %~dp0..
set ROOT=%cd%
set SK=%ROOT%\tools\CXEX_Compiler\kernel.xksk

set SIGNFLAG=-DSIGN=OFF
if exist "%SK%" (
    set SIGNFLAG=-DSIGN=ON
    echo   signing key found - artifacts will be SIGNED
) else (
    echo   no signing key - building UNSIGNED ^(run makekeys.py kernel to enable^)
)

echo [1/3] Cleaning build\ ...
if exist build rmdir /s /q build
mkdir build
cd build

echo [2/3] Configuring CMake %SIGNFLAG% ...
cmake .. -G "NMake Makefiles" %SIGNFLAG%
if errorlevel 1 ( echo [ERROR] CMake configure failed. & exit /b 1 )

echo [3/3] Building ...
cmake --build .
if errorlevel 1 ( echo [ERROR] Build failed. & exit /b 1 )

cd /d %ROOT%
echo =====================================
echo   Done.
echo   kernel : dist\CXK_x86_32\packages\kernel.xkex
echo   exec   : dist\CXK_x86_32\packages\executive.xoex
echo   disk   : dist\CXK_x86_32\images\cxk_disk.img
echo =====================================
endlocal