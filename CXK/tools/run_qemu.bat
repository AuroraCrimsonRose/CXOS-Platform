@echo off
setlocal

echo =====================================
echo   CXK - RUN (QEMU)
echo =====================================

cd /d %~dp0..

set BOOT=dist\CXK_x86_32\cxk_x86_32.img
set FS=dist\CXK_x86_32\cxk_filesystem.img
set USB=dist\CXK_x86_32\cxk_usb.img

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

REM Default machine (i440FX) - the known-good boot environment.
REM   e1000        : NIC for networking dev
REM   pci-ohci     : OHCI USB controller (id=ohci) for the USB stack
REM   usb-storage  : a virtual USB flash drive backed by the USB image, attached
REM                  to the OHCI controller's bus - the device the USB driver
REM                  will eventually enumerate and read as EXT-USB0.
REM -netdev user is built-in SLIRP (no host setup).

qemu-system-i386 -m 4G ^
    -drive format=raw,file=%BOOT%,if=ide,index=0 ^
    -drive format=raw,file=%FS%,if=ide,index=1 ^
    -netdev user,id=net0 ^
    -device e1000,netdev=net0 ^
    -device pci-ohci,id=ohci ^
    -drive id=usbdisk,format=raw,file=%USB%,if=none ^
    -device usb-storage,bus=ohci.0,drive=usbdisk

endlocal