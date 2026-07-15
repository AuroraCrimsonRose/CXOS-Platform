@echo off
setlocal
REM /CXK/tools/build.bat
REM Aurora Tejeda / CATX SYSTEMS LLC
REM
REM Clean build of CXK via CMake. The CXEX toolchain (package/sign/image) runs
REM through tools\cxk.exe from CMake's custom commands - no Python. If the signing
REM key is present, artifacts are SIGNED (-DSIGN=ON); otherwise UNSIGNED.
REM
REM Outputs:
REM   dist\CXK_x86_32\packages\kernel.xkex      (signed if key present)
REM   dist\CXK_x86_32\packages\executive.xoex   (signed if key present)
REM   dist\CXK_x86_32\images\cxk_disk.img        (unified: boot + stage + system)
REM
REM One-time key setup (enables signing):
REM   tools\cxk.exe keygen tools\kernel
REM   tools\cxk.exe embed tools\kernel.xkpk kernel\lib\format\trusted_key.c cxos_trusted_key --extern

echo =====================================
echo   CXK - BUILD
echo =====================================

cd /d %~dp0..
set ROOT=%cd%
set CXK=%ROOT%\tools\cxk.exe
set SK=%ROOT%\tools\kernel.xksk
set CMAKEDIR=%ROOT%\tools\cmake

if not exist "%CXK%" (
    echo [ERROR] Toolchain missing: %CXK%
    echo         Publish cxk.exe from the DevKit and copy it into tools\.
    exit /b 1
)

set SIGNFLAG=-DSIGN=OFF
if exist "%SK%" (
    set SIGNFLAG=-DSIGN=ON
    echo   signing key found - artifacts will be SIGNED
) else (
    echo   no signing key - building UNSIGNED ^(run: tools\cxk.exe keygen tools\kernel^)
)

echo [1/3] Cleaning build\ ...
if exist build rmdir /s /q build
mkdir build

echo [pre-flight] Verifying source list ...
"%CXK%" check "%CMAKEDIR%\CMakeLists.txt"
if errorlevel 1 ( echo [ERROR] Source pre-flight failed. & exit /b 1 )

echo [2/3] Configuring CMake %SIGNFLAG% ...
cmake -S "%CMAKEDIR%" -B build -G "NMake Makefiles" %SIGNFLAG%
if errorlevel 1 ( echo [ERROR] CMake configure failed. & exit /b 1 )

echo [3/3] Building ...
cmake --build build
if errorlevel 1 ( echo [ERROR] Build failed. & exit /b 1 )

cd /d %ROOT%
echo =====================================
echo   Done.
echo   kernel : dist\CXK_x86_32\packages\kernel.xkex
echo   exec   : dist\CXK_x86_32\packages\executive.xoex
echo   disk   : dist\CXK_x86_32\images\cxk_disk.img
echo =====================================
endlocal