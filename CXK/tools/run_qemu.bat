@echo off
setlocal

echo =====================================
echo   CXK - RUN (QEMU)
echo =====================================

cd /d %~dp0..

set BOOT=dist\CXK_x86_32\images\cxk_x86_32.img
set FS=dist\CXK_x86_32\images\cxk_filesystem.img
set USB=dist\CXK_x86_32\images\cxk_usb.img

if not exist %BOOT% (
    echo ERROR: No boot image found.
    echo Expected: %BOOT%
    exit /b 1
)

echo Boot disk: %BOOT%
echo FS   disk: %FS%
echo USB  disk: %USB%   (virtual USB mass-storage on OHCI)
echo NIC      : e1000   USB: OHCI
echo.

REM ---- ensure the filesystem disk image exists (16 MB, blank; CXFS formats
REM      it on first boot). make_fs_img.ps1 leaves an existing image untouched. ----
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make_fs_img.ps1" -Out "%FS%" -SizeMB 16

REM Default machine (i440FX) - the known-good boot environment.
REM   boot disk : IDE index 0 (primary master) - the kernel image
REM   fs   disk : IDE index 1 (primary slave)  - CXFS data disk (ATA unit 1)
REM   e1000     : NIC for networking dev
REM   pci-ohci  : OHCI USB controller for the USB stack
REM -netdev user is built-in SLIRP (no host setup).

qemu-system-i386 -m 4G ^
    -machine pcspk-audiodev=speaker ^
    -audiodev dsound,id=speaker ^
    -drive format=raw,file=%BOOT%,if=ide,index=0 ^
    -drive format=raw,file=%FS%,if=ide,index=1 ^
    -netdev user,id=net0 ^
    -device e1000,netdev=net0 ^
    -device pci-ohci,id=ohci 

endlocal