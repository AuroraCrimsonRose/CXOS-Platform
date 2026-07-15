@echo off
setlocal
REM CXK - RUN (i440FX/pc legacy: boot disk + CXFS data disk on IDE, e1000, audio).
cd /d %~dp0..
"%cd%\tools\cxk.exe" run "dist\CXK_x86_32\images\cxk_x86_32.img" ^
    -M pc --fs "dist\CXK_x86_32\images\cxk_filesystem.img"
endlocal
