@echo off
setlocal

echo =====================================
echo   CXK - RUN (QEMU q35 + AHCI)
echo =====================================
echo   For AHCI driver development/testing.
echo   Disks on q35's built-in ICH9 AHCI controller (00:1f.2).
echo =====================================

cd /d %~dp0..

set BOOT=dist\CXK_x86_32\cxk_x86_32.img
set FS=dist\CXK_x86_32\cxk_filesystem.img
set USB=dist\CXK_x86_32\cxk_usb.img

if not exist %BOOT% (
    echo ERROR: No boot image found.
    exit /b 1
)

REM q35 (ICH9 AHCI) with both fixed disks on the built-in AHCI, plus an OHCI
REM USB controller + a virtual USB flash drive for USB-stack development.

qemu-system-i386 -m 4G -machine q35 ^
    -drive id=bootdisk,format=raw,file=%BOOT%,if=none ^
    -device ide-hd,drive=bootdisk,bus=ide.0,bootindex=0 ^
    -drive id=fsdisk,format=raw,file=%FS%,if=none ^
    -device ide-hd,drive=fsdisk,bus=ide.2 ^
    -netdev user,id=net0 ^
    -device e1000,netdev=net0 ^
    -device pci-ohci,id=ohci ^
    -drive id=usbdisk,format=raw,file=%USB%,if=none ^
    -device usb-storage,bus=ohci.0,drive=usbdisk

endlocal