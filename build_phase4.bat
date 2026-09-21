@echo off
setlocal
rem Build the Phase 4 Tier 1.1 probe (phase4\tier11probe.cpp) with MSVC.
rem
rem   build_phase4.bat [path\to\agility]
rem or set AGILITY_SDK_DIR. DXC comes from DXC_SDK_DIR or C:\DW\DXC.
rem
rem Output: phase4out\tier11probe.exe
rem
rem NOTE the separate output directory. The repo root contains our proxy
rem d3d12.dll, and an exe built there would load the proxy instead of the system
rem runtime. This probe is meant to measure the REAL runtime, so it gets a clean
rem directory with no proxy in it. To deliberately run the probe THROUGH the
rem proxy later, copy d3d12.dll into phase4out\ and run it again.

set "DXC=%DXC_SDK_DIR%"
if "%DXC%"=="" set "DXC=C:\DW\DXC"
set "AGILITY=%~1"
if "%AGILITY%"=="" set "AGILITY=%AGILITY_SDK_DIR%"
if "%AGILITY%"=="" set "AGILITY=C:\DW\microsoft.direct3d.d3d12.1.619.5"
if not exist "%AGILITY%\build\native\include\d3d12.h" (
  echo d3d12.h not found under "%AGILITY%\build\native\include".
  echo Pass the Agility SDK path, e.g.:  build_phase4.bat C:\path\to\agility
  exit /b 1
)

call "%~dp0setup_msvc.bat" || exit /b 1

set "OUT=%~dp0phase4out"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

cl /nologo /EHsc /std:c++17 /O2 /W4 ^
   /I "%AGILITY%\build\native\include" /I "%DXC%\inc" ^
   /Fo:"%OUT%\obj\\" ^
   phase4\tier11probe.cpp /Fe:"%OUT%\tier11probe.exe" ^
   /link d3d12.lib dxgi.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem Agility runtime must sit in .\D3D12\ next to the exe.
if not exist "%OUT%\D3D12" mkdir "%OUT%\D3D12"
copy /y "%AGILITY%\build\native\bin\x64\D3D12Core.dll"      "%OUT%\D3D12\" >nul
copy /y "%AGILITY%\build\native\bin\x64\d3d12SDKLayers.dll" "%OUT%\D3D12\" >nul
rem DXC runtime for the shader compiles, and dxil.dll so DXC can sign them.
copy /y "%DXC%\bin\x64\dxcompiler.dll" "%OUT%\" >nul
copy /y "%DXC%\bin\x64\dxil.dll"       "%OUT%\" >nul

echo.
echo Built phase4out\tier11probe.exe
echo Run:  phase4out\tier11probe.exe          both adapters
echo       phase4out\tier11probe.exe warp     ground truth only
echo       phase4out\tier11probe.exe hw       the GTX 1070 only
if not exist "%OUT%\D3D12\D3D12Core.dll" echo WARNING: D3D12Core.dll missing.
if not exist "%OUT%\dxil.dll" echo WARNING: dxil.dll missing, shader signing will fail.
