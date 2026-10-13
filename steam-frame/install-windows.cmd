@echo off
rem install-windows.cmd - double-click it to install, update or remove CitraVR - Steam Frame
rem Edition on a Steam Frame from Windows 10/11. It runs install-windows.ps1 with PowerShell: the
rem copy next to it when there is one, otherwise the latest from GitHub.
setlocal
set "script=%~dp0install-windows.ps1"
if exist "%script%" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%script%"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072; irm https://raw.githubusercontent.com/SPD13/CitraVR-steam-frame/master/steam-frame/install-windows.ps1 | iex"
)
echo.
pause
