@echo off
rem WizBar console launcher (shows logs - useful while tweaking).
set APPDIR=%~dp0..
if exist "%APPDIR%\node_modules\electron\dist\electron.exe" (
  start "" "%APPDIR%\node_modules\electron\dist\electron.exe" "%APPDIR%"
) else (
  cd /d "%APPDIR%" && npm start
)
