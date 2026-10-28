@echo off
setlocal
cd /d "%~dp0\..\.."

echo [VinylDeck] 合并完整固件...

where py >nul 2>nul
if not errorlevel 1 (
    py -3 "%~dp0merge_fakepod_full.py"
) else (
    python "%~dp0merge_fakepod_full.py"
)

if errorlevel 1 (
    echo.
    echo [VinylDeck] 合并失败，请检查上面的错误信息。
    pause
    exit /b 1
)

echo.
echo [VinylDeck] 完成。
pause
