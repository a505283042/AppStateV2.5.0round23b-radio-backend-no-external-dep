@echo off
setlocal EnableExtensions

set "SCRIPT=%~dp0ESP32_NAS_Music_Scanner_UI_R56.ps1"

if not exist "%SCRIPT%" (
    echo ERROR: Scanner script was not found:
    echo %SCRIPT%
    echo.
    pause
    exit /b 2
)

echo Starting ESP32 NAS Music Scanner UI R56...
echo Script: %SCRIPT%
echo.

powershell.exe -NoLogo -NoProfile -STA -ExecutionPolicy Bypass -File "%SCRIPT%"
set "EXIT_CODE=%ERRORLEVEL%"

echo.
echo Finished. Exit code: %EXIT_CODE%
pause
exit /b %EXIT_CODE%
