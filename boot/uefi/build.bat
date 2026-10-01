@echo off
setlocal
REM /boot/uefi/build.bat
REM Aurora Tejeda / CATX SYSTEMS LLC
REM
REM Builds the UEFI boot stub with MSVC. Run from an x64 Native Tools / Developer
REM Command Prompt. This is now the ONLY part of the repository that needs MSVC:
REM the OS build moved to clang + ld.lld + Ninja (HARDENING_PLAN D5) and runs in
REM a plain shell. This stub follows with `cxk uefi build` (clang + lld-link).
REM
REM No new toolchain: link.exe produces UEFI applications natively via
REM /SUBSYSTEM:EFI_APPLICATION. This is how EDK2 builds on Windows.
REM
REM The kernel's cross target (clang --target=i686-elf) CANNOT build this, and is
REM not supposed to: the kernel is 32-bit ELF, a UEFI application is 64-bit PE32+.
REM Different targets, separate artifacts.
REM
REM   /GS-       no stack cookies (there is no CRT to provide them)
REM   /Gs32768   effectively disables stack probes; firmware interrupts share
REM              this stack, so the red zone / probe assumptions do not hold
REM   /kernel    no C++ exception machinery
REM
REM Output: BOOTX64.EFI  -> copy to the ESP as \EFI\BOOT\BOOTX64.EFI

cd /d %~dp0
set OUT=BOOTX64.EFI

echo [1/2] Compiling cxboot.c ...
cl /c /nologo /W4 /GS- /Gs32768 /kernel /I..\..\abi cxboot.c /Fo:cxboot.obj
if errorlevel 1 ( echo [ERROR] compile failed. & exit /b 1 )

echo [2/2] Linking %OUT% ...
link /NOLOGO /SUBSYSTEM:EFI_APPLICATION /ENTRY:efi_main /NODEFAULTLIB ^
     /MACHINE:X64 /OUT:%OUT% cxboot.obj
if errorlevel 1 ( echo [ERROR] link failed. & exit /b 1 )

echo.
echo   Done: %~dp0%OUT%
echo   Copy to an ESP as \EFI\BOOT\BOOTX64.EFI to boot it.
endlocal
