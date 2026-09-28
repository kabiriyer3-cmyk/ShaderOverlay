@echo off
cd /d "%~dp0"
where g++ >nul 2>nul
if not %errorlevel%==0 (
  echo g++ not found. Download w64devkit from https://github.com/skeeto/w64devkit/releases
  echo unzip it, then run w64devkit.exe, cd to this folder and run build_gcc.bat
  pause & exit /b 1
)
g++ -O2 -std=c++17 -municode -mwindows -DUNICODE -D_UNICODE ShaderOverlay.cpp -o ShaderOverlay.exe -ld3d11 -ldxgi -ld3dcompiler -lgdiplus -lshell32 -lole32 -lgdi32 -luser32 -ldwmapi -static
if %errorlevel%==0 (echo Built ShaderOverlay.exe) else (echo Build FAILED, send me the errors)
pause
