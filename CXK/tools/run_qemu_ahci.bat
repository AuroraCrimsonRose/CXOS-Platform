@echo off
setlocal
REM CXK - RUN (q35 + AHCI, unified disk). Replaces the old raw qemu invocation.
cd /d %~dp0..
"%cd%\tools\cxk.exe" run "dist\CXK_x86_32\images\cxk_disk.img"
endlocal
