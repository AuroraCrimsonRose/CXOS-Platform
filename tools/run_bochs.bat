@echo off
setlocal
REM CXK - RUN (Bochs). cxk generates the bochsrc next to the image.
cd /d %~dp0..
"%cd%\tools\cxk.exe" run "dist\CXK_x86_32\images\cxk_disk.img" -e bochs
endlocal
