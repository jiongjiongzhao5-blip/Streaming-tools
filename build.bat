@echo off
setlocal
set "ROOT=%~dp0"

if "%QT_DIR%"=="" set "QT_DIR=C:\Qt\6.8.2\msvc2022_64"
if "%VCPKG_DIR%"=="" set "VCPKG_DIR=C:\vcpkg"

set "LVT_QT=%QT_DIR%"
set "LVT_VCPKG=%VCPKG_DIR%"

if not exist "%LVT_QT%\lib\cmake\Qt6\Qt6Config.cmake" (
  echo [error] Qt6 not found at "%LVT_QT%"
  goto :fail
)
if not exist "%LVT_VCPKG%\scripts\buildsystems\vcpkg.cmake" (
  echo [error] vcpkg not found at "%LVT_VCPKG%"
  goto :fail
)
if not exist "%LVT_VCPKG%\installed\x64-windows\include\libavcodec\avcodec.h" (
  echo [error] FFmpeg is missing in vcpkg.
  echo         Run:  "%LVT_VCPKG%\vcpkg" install "ffmpeg[gpl,x264]:x64-windows"
  goto :fail
)

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
      -property installationPath`) do (
    if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
  )
)
if not defined VCVARS (
  for %%e in (Community Professional Enterprise BuildTools) do (
    if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat" (
      set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%e\VC\Auxiliary\Build\vcvars64.bat"
    )
  )
)
if not defined VCVARS (
  echo [error] Visual Studio 2022 C++ tools not found.
  goto :fail
)

echo [1/3] Configuring...
call "%VCVARS%" >nul
cmake -S "%ROOT%." -B "%ROOT%build" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=%LVT_VCPKG%/scripts/buildsystems/vcpkg.cmake ^
  -DCMAKE_PREFIX_PATH=%LVT_QT% ^
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if errorlevel 1 goto :fail

echo.
echo [2/3] Building...
cmake --build "%ROOT%build"
if errorlevel 1 goto :fail

echo.
echo [3/3] Deploying Qt runtime (windeployqt)...
"%LVT_QT%\bin\windeployqt.exe" --no-translations "%ROOT%build\LightLiveStreamer.exe" >nul 2>&1

echo.
echo [done] Executable: %ROOT%build\LightLiveStreamer.exe
exit /b 0

:fail
echo.
echo [failed] Build did not finish. Read the messages above.
exit /b 1
