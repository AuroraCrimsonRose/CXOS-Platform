@echo off
REM /boot/uefi/secureboot.bat
REM Aurora Tejeda / CATX Systems
REM
REM Generate a CXK Secure Boot key set, enroll it into an OVMF variable store,
REM sign the UEFI stub with it, and boot the result under QEMU - twice.
REM
REM ---- Why this exists ------------------------------------------------------
REM
REM cxboot.c reads the firmware's SecureBoot variable and sets
REM CXBI_FLAG_SECURE_BOOT. That line cannot be tested with Secure Boot off (it
REM reports off, which proves nothing) and it cannot be tested on an OVMF build
REM carrying the Microsoft keys either - firmware refuses to launch an unsigned
REM stub at all, so the code never runs. The only way to exercise it is to
REM become the platform owner: enroll our own PK/KEK/db, sign the stub with our
REM db key, and let firmware verify it.
REM
REM That is not just a test rig. It is the same procedure for running CXK on
REM your own Secure Boot hardware, and the same key material that would let the
REM stub be a real root of trust: firmware verifies the stub, the stub verifies
REM kernel.xkex, and CXBI_FLAG_KERNEL_VERIFIED means something.
REM
REM ---- Requirements ---------------------------------------------------------
REM
REM   tools\cxk.exe    does the key generation, the variable store, and the
REM                    Authenticode signing - no OpenSSL, no Python, no Windows
REM                    SDK, no signtool.
REM   qemu-system-x86_64.exe, and the OVMF firmware that ships with it under
REM                    QEMU's share\ directory.
REM
REM Pass --firmware-dir to cxk if your OVMF images live somewhere unusual.

setlocal
echo =====================================
echo   CXK - SECURE BOOT TEST
echo =====================================

cd /d %~dp0
set HERE=%cd%
cd /d %~dp0..\..
set ROOT=%cd%
set CXK=%ROOT%\tools\cxk.exe
cd /d %HERE%

set STUB=%1
if "%STUB%"=="" set STUB=BOOTX64.EFI

if not exist "%CXK%" (
    echo [ERROR] Toolchain missing: %CXK%
    echo         cxk.exe is not committed. Download it from a release, or publish it from devkit\, into tools\.
    exit /b 1
)

REM Probed rather than called outright, the same way build.bat probes check-abi:
REM a cxk.exe published before these commands existed would fail on an unknown
REM command with nothing useful to say.
"%CXK%" secureboot --help >nul 2>&1
if errorlevel 1 (
    echo [ERROR] this cxk.exe predates the secureboot commands.
    echo         Republish cxk.exe from the DevKit and copy it into tools\.
    exit /b 1
)

if not exist "%STUB%" (
    echo [ERROR] '%STUB%' not found - build the stub first ^(build.bat^).
    exit /b 1
)

REM Kept if they already exist: regenerating would invalidate the variable store
REM a previous run enrolled, and there is no reason to churn them.
if not exist sbkeys\db.pfx (
    echo [1/3] Generating PK/KEK/db ...
    "%CXK%" secureboot keygen
    if errorlevel 1 exit /b 1
) else (
    echo [1/3] Reusing the key set in sbkeys\
)

echo [2/3] Enrolling into an OVMF variable store ...
"%CXK%" secureboot varstore
if errorlevel 1 exit /b 1

echo [3/3] Booting signed and unsigned under enforced Secure Boot ...
"%CXK%" secureboot test "%STUB%"
if errorlevel 1 (
    echo.
    echo [ERROR] Secure Boot test FAILED - see above.
    exit /b 1
)

echo.
echo   sbkeys\CXK_VARS.fd  - pass to QEMU as pflash unit 1 to boot signed builds
echo   sbkeys\db.pfx       - sign further builds:
echo       tools\cxk.exe secureboot sign in.efi out.efi
echo   sbkeys\*.cer        - DER, for enrolling on real hardware from firmware setup
endlocal
