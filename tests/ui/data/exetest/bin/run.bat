@echo off
echo cwd=%CD%> "%CABINE_UI_WORK%\runout.txt"
dir /s /b "%~dp0.." >> "%CABINE_UI_WORK%\runout.txt"
