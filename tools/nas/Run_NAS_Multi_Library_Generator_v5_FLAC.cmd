@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0make_net_music_multi_sources_raw_utf8_v5_flac.ps1"
set "RC=%ERRORLEVEL%"
echo.
echo Finished. Exit code: %RC%
echo Log file: %USERPROFILE%\Desktop\net_music_generator.log
pause
exit /b %RC%
