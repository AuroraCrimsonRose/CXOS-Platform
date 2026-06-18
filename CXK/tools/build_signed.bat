@echo off
setlocal
REM /CXK/tools/build_signed.bat
REM Aurora Tejeda / CATX SYSTEMS LLC
REM Build the kernel, convert it to CXEX (.xkex), then sign it.
REM
REM Uses the project's .venv Python (one level above the inner CXK project,
REM i.e. ...\CXK\.venv) so it works whether or not the venv is activated.
REM
REM ONE-TIME SETUP (run once, then guard the .xksk private key):
REM   "%PY%" tools\CXEX_Compiler\makekeys.py kernel
REM   (move kernel.xksk somewhere safe; it must NEVER ship / be committed)

echo =====================================
echo   CXK - BUILD + SIGN
echo =====================================

cd /d %~dp0..
set ROOT=%cd%
set COMP=%ROOT%\tools\CXEX_Compiler
set ELF=%ROOT%\build\kernel.elf
set XKEX=%ROOT%\dist\CXK_x86_32\packages\cxk.xkex
set SK=%COMP%\kernel.xksk
set PK=%COMP%\kernel.xkpk

REM ---- locate the venv Python ----
REM .venv is in the OUTER folder (parent of this inner CXK project root).
set PY=%ROOT%\..\.venv\Scripts\python.exe
if not exist "%PY%" (
    REM fallback: a .venv inside the project root
    if exist "%ROOT%\.venv\Scripts\python.exe" set PY=%ROOT%\.venv\Scripts\python.exe
)
if not exist "%PY%" (
    echo [ERROR] venv Python not found.
    echo         looked for: %ROOT%\..\.venv\Scripts\python.exe
    echo         activate your venv or edit PY in this script.
    exit /b 1
)
echo Using Python: %PY%

REM ---- 1. build (reuse the normal CMake flow) ----
echo [1/4] Building kernel...
if exist build rmdir /s /q build
mkdir build
cd build
cmake .. -G "NMake Makefiles"
if errorlevel 1 ( echo [ERROR] CMake failed. & exit /b 1 )
cmake --build .
if errorlevel 1 ( echo [ERROR] Build failed. & exit /b 1 )
cd /d %ROOT%

if not exist "%ELF%" ( echo [ERROR] kernel.elf not found at %ELF% & exit /b 1 )

REM ---- 2. convert ELF -> CXEX ----
echo [2/4] Converting kernel.elf -^> CXEX...
if not exist "%ROOT%\dist\CXK_x86_32\packages" mkdir "%ROOT%\dist\CXK_x86_32\packages"
"%PY%" "%COMP%\mkcxes.py" "%ELF%" "%XKEX%" --type kernel
if errorlevel 1 ( echo [ERROR] CXEX conversion failed. & exit /b 1 )

REM ---- 3. sign (only if keys exist) ----
echo [3/4] Signing CXEX...
if not exist "%SK%" (
    echo   [SKIP] no signing key at %SK%
    echo          run: "%PY%" tools\CXEX_Compiler\makekeys.py kernel
    echo          ^(unsigned .xkex was still produced^)
    goto done
)
if not exist "%PK%" ( echo [ERROR] public key missing: %PK% & exit /b 1 )
"%PY%" "%COMP%\signcxex.py" "%XKEX%" "%SK%" "%PK%"
if errorlevel 1 ( echo [ERROR] signing failed. & exit /b 1 )

:done
echo [4/4] Done.
echo =====================================
echo   Output: %XKEX%
echo =====================================
endlocal