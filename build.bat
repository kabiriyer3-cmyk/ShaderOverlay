@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if %errorlevel%==0 goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto novs
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if not %errorlevel%==0 goto novs

:build
cl /nologo /O2 /EHsc /std:c++17 /DUNICODE /D_UNICODE ShaderOverlay.cpp /Fe:ShaderOverlay.exe /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib d3dcompiler.lib gdiplus.lib shell32.lib user32.lib gdi32.lib dwmapi.lib runtimeobject.lib
if %errorlevel%==0 (echo. & echo Built ShaderOverlay.exe) else (echo. & echo Build FAILED, copy the errors above and send them to me)
pause
exit /b

:novs
echo.
echo Visual Studio C++ compiler not found.
echo Install "Build Tools for Visual Studio 2022" from https://visualstudio.microsoft.com/downloads/
echo and tick "Desktop development with C++". Then run build.bat again.
pause
exit /b 1
