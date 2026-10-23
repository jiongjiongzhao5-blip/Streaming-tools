@echo off
setlocal
set "ROOT=%~dp0"

if not exist "%ROOT%build\LightLiveStreamer.exe" (
  echo [error] build\LightLiveStreamer.exe not found. Run build.bat first.
  goto :fail
)

echo [1/2] Cleaning dist...
if exist "%ROOT%dist" rmdir /s /q "%ROOT%dist"

echo [2/2] Installing runtime...
cmake --install "%ROOT%build" --prefix "%ROOT%dist"
if errorlevel 1 goto :fail

echo.
echo [done] %ROOT%dist
echo.
echo Contents:
dir /b "%ROOT%dist"
echo.
echo Copy the whole dist folder to another machine and run LightLiveStreamer.exe
exit /b 0

:fail
echo.
echo [failed] Packaging did not finish.
exit /b 1
