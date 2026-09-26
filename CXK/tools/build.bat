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

REM The X compiler carries a hand-maintained copy of abi\cxk_abi.h as its prelude,
REM in the DevKit repo. If they drift, the userland fails to compile with confusing
REM "undefined name" errors a long way from the cause - so catch it here instead.
REM
REM Probed rather than called outright: a cxk.exe published before check-abi existed
REM would fail on an unknown command and abort a build that is otherwise fine. So an
REM older toolchain warns and skips, and the check switches itself on once cxk.exe is
REM republished from the DevKit. Real drift is still a hard failure.
echo [pre-flight] Verifying X ABI prelude matches cxk_abi.h ...
"%CXK%" check-abi --help >nul 2>&1
if errorlevel 1 goto :abi_skip
"%CXK%" check-abi "%ROOT%\abi\cxk_abi.h"
if errorlevel 1 ( echo [ERROR] ABI pre-flight failed. & exit /b 1 )
goto :abi_done
:abi_skip
echo   [warn] this cxk.exe predates check-abi - skipping the ABI check.
echo          Republish cxk.exe from the DevKit to enable it.
:abi_done

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