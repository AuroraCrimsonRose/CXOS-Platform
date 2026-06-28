@echo off
REM Run from the repo root:  docs\build-docs.cmd
dotnet tool restore || exit /b 1
dotnet docfx docs/docfx.json --serve
pause