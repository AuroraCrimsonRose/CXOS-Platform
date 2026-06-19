@echo off
setlocal

echo =====================================
echo   CXK - RUN (QEMU q35 + AHCI test)
echo =====================================
echo   For AHCI driver development/testing.
echo   boot + FS disks on the ICH9 SATA/AHCI controller (q35 built-in).
echo =====================================

cd /d %~dp0..

set BOOT=dist\CXK_x86_32\images\cxk_x86_32.img
set FS=dist\CXK_x86_32\images\cxk_filesystem.img

if not exist %BOOT% (
    echo ERROR: No boot image found. Build first.
    exit /b 1
)

REM ensure the filesystem image exists (blank; CXFS formats on first boot)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make_fs_img.ps1" -Out "%FS%" -SizeMB 16

REM q35's built-in ICH9 controller is an AHCI/SATA HBA (PCI 00:1f.2, class
REM 0x01 subclass 0x06 prog-if 0x01 - exactly what ahci_init's pci_find looks
REM for). Attaching disks via ide-hd to its buses presents them through AHCI.
REM   boot disk -> bus ide.0 (SATA port 0), bootable
REM   fs   disk -> bus ide.2 (SATA port 2) - CXFS data disk
REM On this machine there is NO legacy IDE, so ATA PIO finds nothing and ALL
REM disk access goes through the AHCI driver - a true end-to-end AHCI test.

qemu-system-i386 -m 4G -machine q35 ^
    -machine pcspk-audiodev=speaker ^
    -audiodev dsound,id=speaker ^
    -drive id=bootdisk,format=raw,file=%BOOT%,if=none ^
    -device ide-hd,drive=bootdisk,bus=ide.0,bootindex=0 ^
    -drive id=fsdisk,format=raw,file=%FS%,if=none ^
    -device ide-hd,drive=fsdisk,bus=ide.2 ^
    -netdev user,id=net0 ^
    -device e1000,netdev=net0

endlocal