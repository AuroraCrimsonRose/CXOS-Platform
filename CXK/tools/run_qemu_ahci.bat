@echo off
setlocal
REM CXK - RUN (q35 + AHCI, unified disk) WITH an e1000 NIC.
REM
REM Invoked directly rather than through `cxk run` so the NIC options are
REM explicit and version-independent. QEMU user-mode networking puts the guest
REM on 10.0.2.0/24:
REM     guest   10.0.2.15      gateway/host  10.0.2.2      DNS  10.0.2.3
REM so after boot:
REM     cxk> setip 10.0.2.15
REM     cxk> ping 10.0.2.2

cd /d %~dp0..
set DISK=dist\CXK_x86_32\images\cxk_disk.img

if not exist %DISK% (
    echo ERROR: %DISK% not found. Run tools\build.bat first.
    exit /b 1
)

echo Launching QEMU (q35 + AHCI + e1000)...
qemu-system-i386 -m 4G -machine q35 ^
    -machine pcspk-audiodev=speaker ^
    -audiodev dsound,id=speaker ^
    -drive id=cxkdisk,format=raw,file=%DISK%,if=none ^
    -device ide-hd,drive=cxkdisk,bus=ide.0,bootindex=0 ^
    -netdev user,id=net0 ^
    -device e1000,netdev=net0 ^
    -object filter-dump,id=dump0,netdev=net0,file=dist\net-trace.pcap

endlocal
