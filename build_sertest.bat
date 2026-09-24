@echo off
setlocal
rem Build the SER speed test (bench\sertest.cpp) with MSVC.
rem
rem   build_sertest.bat [path\to\agility]
rem or set AGILITY_SDK_DIR. DXC comes from DXC_SDK_DIR or C:\DW\DXC.
rem
rem Output: benchout\sertest.exe, in a folder with no proxy d3d12.dll, so it
rem measures the real runtime and driver.

set "DXC=%DXC_SDK_DIR%"
if "%DXC%"=="" set "DXC=C:\DW\DXC"
set "AGILITY=%~1"
if "%AGILITY%"=="" set "AGILITY=%AGILITY_SDK_DIR%"
if "%AGILITY%"=="" set "AGILITY=C:\DW\microsoft.direct3d.d3d12.1.619.5"
if not exist "%AGILITY%\build\native\include\d3d12.h" (
  echo d3d12.h not found under "%AGILITY%\build\native\include".
  exit /b 1
)

call "%~dp0setup_msvc.bat" || exit /b 1

set "OUT=%~dp0benchout"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

cl /nologo /EHsc /std:c++17 /O2 /W4 ^
   /I "%AGILITY%\build\native\include" /I "%DXC%\inc" ^
   /Fo:"%OUT%\obj\\" ^
   "%~dp0bench\sertest.cpp" /Fe:"%OUT%\sertest.exe" ^
   /link d3d12.lib dxgi.lib user32.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1

if not exist "%OUT%\D3D12" mkdir "%OUT%\D3D12"
copy /y "%AGILITY%\build\native\bin\x64\D3D12Core.dll"      "%OUT%\D3D12\" >nul
copy /y "%AGILITY%\build\native\bin\x64\d3d12SDKLayers.dll" "%OUT%\D3D12\" >nul
copy /y "%DXC%\bin\x64\dxcompiler.dll" "%OUT%\" >nul
copy /y "%DXC%\bin\x64\dxil.dll"       "%OUT%\" >nul

echo.
echo Built benchout\sertest.exe
echo Run:  benchout\sertest.exe [--m 32] [--res 1024] [--iters 15] [--k 8,32,128,512]
