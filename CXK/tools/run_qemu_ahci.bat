@echo off
setlocal

echo =====================================
echo   CXK - RUN (QEMU q35 + AHCI)
echo =====================================
echo   Single unified disk (boot + XBPT + system) on the ICH9 AHCI HBA.
echo =====================================

cd /d %~dp0..

set DISK=dist\CXK_x86_32\images\cxk_disk.img

if not exist %DISK% (
    echo ERROR: %DISK% not found. Run tools\build.bat first.
    exit /b 1
)

REM q35's built-in ICH9 controller is an AHCI/SATA HBA. Attaching the unified
REM disk via ide-hd to bus ide.0 presents it through AHCI, bootable. Stage 1
REM loads stage 2 from LBA 2; stage 2 reads the XBPT at LBA 1, finds the BOOT
REM partition, and loads the kernel from it. The kernel then finds the SYSTEM
REM partition, installs /System from STAGE on first boot, and launches the
REM signed executive.
qemu-system-i386 -m 4G -machine q35 ^
    -machine pcspk-audiodev=speaker ^
    -audiodev dsound,id=speaker ^
    -drive id=cxkdisk,format=raw,file=%DISK%,if=none ^
    -device ide-hd,drive=cxkdisk,bus=ide.0,bootindex=0 ^
    -netdev user,id=net0 ^
    -device e1000,netdev=net0

endlocal