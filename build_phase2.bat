@echo off
setlocal
rem Build the Phase 2 hand-lowering test (phase2\raytest.cpp) with MSVC.
rem Run from an "x64 Native Tools Command Prompt for VS".
rem
rem   build_phase2.bat C:\path\to\agility        (dir with build\native\include\d3d12.h)
rem or set AGILITY_SDK_DIR, then: build_phase2.bat
rem
rem DXC is taken from DXC_SDK_DIR or C:\DW\DXC.
rem If your Agility d3d12.h does not define D3D12_SDK_VERSION, the app defaults
rem the runtime version to 614; override by editing the /D line below to match
rem your SDK, e.g. /DDXRTEST_AGILITY_VERSION=616

set "DXC=%DXC_SDK_DIR%"
if "%DXC%"=="" set "DXC=C:\DW\DXC"
set "AGILITY=%~1"
if "%AGILITY%"=="" set "AGILITY=%AGILITY_SDK_DIR%"
if "%AGILITY%"=="" (
  echo Provide the Agility SDK path, e.g.:  build_phase2.bat C:\path\to\agility
  echo (the folder that contains build\native\include\d3d12.h and build\native\bin\x64\D3D12Core.dll^)
  echo Get it via NuGet: nuget install Microsoft.Direct3D.D3D12
  exit /b 1
)
if not exist "%AGILITY%\build\native\include\d3d12.h" (
  echo d3d12.h not found under "%AGILITY%\build\native\include".
  exit /b 1
)
where cl >nul 2>nul || (
  echo cl.exe not found - open "x64 Native Tools Command Prompt for VS" first.
  exit /b 1
)

cl /nologo /EHsc /std:c++17 /I "%AGILITY%\build\native\include" /I "%DXC%\inc" ^
   phase2\raytest.cpp /Fe:raytest.exe /link d3d12.lib dxgi.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem Agility runtime must sit in .\D3D12\ next to the exe.
if not exist D3D12 mkdir D3D12
if exist "%AGILITY%\build\native\bin\x64\D3D12Core.dll"     copy /y "%AGILITY%\build\native\bin\x64\D3D12Core.dll" D3D12\ >nul
if exist "%AGILITY%\build\native\bin\x64\d3d12SDKLayers.dll" copy /y "%AGILITY%\build\native\bin\x64\d3d12SDKLayers.dll" D3D12\ >nul
rem DXC runtime for shader compile + signing.
if exist "%DXC%\bin\x64\dxcompiler.dll" copy /y "%DXC%\bin\x64\dxcompiler.dll" . >nul
if exist "%DXC%\bin\x64\dxil.dll"       copy /y "%DXC%\bin\x64\dxil.dll" . >nul

echo.
echo Built raytest.exe. Run:  raytest.exe        (WARP RayQuery vs HW TraceRay, then diff)
if not exist D3D12\D3D12Core.dll echo WARNING: D3D12\D3D12Core.dll missing - copy it from the Agility SDK bin\x64.
if not exist dxil.dll echo WARNING: dxil.dll missing - copy a signed one next to the exe.
