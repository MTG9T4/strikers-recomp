@echo off
rem Play the developer build on this PC (your own save in build\play-userdata, kept apart from the test saves).
rem F1 opens the PC settings (graphics, controls, online). Close the window to quit.
setlocal
set "ROOT=%~dp0.."
set "STRIKERS_ROMFS=%ROOT%\build\dump\update\romfs"
set "STRIKERS_USERDATA=%ROOT%\build\play-userdata"
if not exist "%STRIKERS_USERDATA%" mkdir "%STRIKERS_USERDATA%"
cd /d "%ROOT%\build\bin"
start "" strikers_native.exe "%ROOT%\build\dump\update\elf\main.elf"
endlocal
