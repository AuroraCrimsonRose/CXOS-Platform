@echo off
setlocal EnableExtensions

echo =====================================
echo        CXOS BOCHS LAUNCHER
echo =====================================

REM ---- Project root ----
cd /d %~dp0..
set ROOT=%cd%

REM ---- Absolute paths ----
set BOCHS_EXE="C:\Program Files\Bochs-3.0\bochs.exe"
set BOCHS_ROMDIR="C:\Program Files\Bochs-3.0"

set BOOT_BIN="C:\Users\Aurora\Documents\VS Code\CXOS Lite\CXLite\build\boot.bin"
set BOCHSRC="%ROOT%\bochsrc2.txt"

set BIOS_ROM=%BOCHS_ROMDIR%\BIOS-bochs-latest
set VGA_ROM=%BOCHS_ROMDIR%\VGABIOS-lgpl-latest.bin

REM ---- Checks ----
if not exist %BOCHS_EXE% (
    echo [ERROR] Bochs not found: %BOCHS_EXE%
    exit /b 1
)

if not exist %BIOS_ROM% (
    echo [ERROR] BIOS ROM not found: %BIOS_ROM%
    exit /b 1
)

if not exist %VGA_ROM% (
    echo [ERROR] VGA ROM not found: %VGA_ROM%
    exit /b 1
)

if not exist %BOOT_BIN% (
    echo [ERROR] boot.bin not found: %BOOT_BIN%
    exit /b 1
)

if not exist %BOCHSRC% (
    echo [ERROR] bochsrc.txt not found: %BOCHSRC%
    exit /b 1
)

echo [OK] All files found
echo [OK] Launching Bochs...

%BOCHS_EXE% -f %BOCHSRC%

echo =====================================
echo BOCHS EXITED
echo =====================================

endlocal