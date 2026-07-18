@echo off
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if %errorlevel%==0 (
    py -3 "%~dp0music_library_scanner.py"
    goto :end
)

where python >nul 2>nul
if %errorlevel%==0 (
    python "%~dp0music_library_scanner.py"
    goto :end
)

echo.
echo 未检测到 Python 3。
echo 请先安装 Python 3.10 或更高版本，并勾选 Add Python to PATH。
echo.
pause

:end
endlocal
