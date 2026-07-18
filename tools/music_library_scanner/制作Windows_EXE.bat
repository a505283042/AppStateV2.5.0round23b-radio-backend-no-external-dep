@echo off
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if not %errorlevel%==0 (
    echo 未检测到 Python 启动器 py.exe。
    pause
    exit /b 1
)

py -3 -m PyInstaller --version >nul 2>nul
if not %errorlevel%==0 (
    echo 正在安装 PyInstaller，仅制作 EXE 时需要...
    py -3 -m pip install --upgrade pyinstaller
    if not %errorlevel%==0 (
        echo PyInstaller 安装失败。
        pause
        exit /b 1
    )
)

py -3 -m PyInstaller ^
  --noconfirm ^
  --clean ^
  --onefile ^
  --windowed ^
  --name ESP32_Music_Library_Scanner ^
  music_library_scanner.py

if %errorlevel%==0 (
    echo.
    echo 制作完成：%~dp0dist\ESP32_Music_Library_Scanner.exe
) else (
    echo.
    echo EXE 制作失败，请查看上方错误。
)
pause
endlocal
