@echo off
setlocal
set "ISLAND_TEST=1"
call "%~dp0build.cmd" %* || exit /b 1
