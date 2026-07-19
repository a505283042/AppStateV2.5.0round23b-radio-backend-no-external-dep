@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0make_net_music_multi_sources_raw_utf8_v4.ps1"
set "EXIT_CODE=%ERRORLEVEL%"
echo.
echo Finished. Exit code: %EXIT_CODE%
echo Log file: %USERPROFILE%\Desktop\net_music_generator.log
echo.
pause
exit /b %EXIT_CODE%
