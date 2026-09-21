@echo off
setlocal
rem Build the Microsoft DXR 1.0 sample "D3D12RaytracingHelloWorld" with cl.exe,
rem straight from the DirectX-Graphics-Samples checkout, with no NuGet restore
rem and without modifying that repo. Output goes to .\sampletest\.
rem
rem   build_sample.bat [path\to\DirectX-Graphics-Samples]
rem
rem The upstream .vcxproj needs NuGet packages (Microsoft.Direct3D.D3D12 1.619.0
rem and WinPixEventRuntime). The sample has no PIX code, only the msbuild import,
rem so we skip PIX entirely and use the Agility SDK already on this machine.
rem
rem This exists to validate the Phase 3a proxy against a real windowed DXR 1.0
rem app: swapchain, Present, continuous frames, Agility SDK runtime.

set "SAMPLES=%~1"
if "%SAMPLES%"=="" set "SAMPLES=C:\DW\DirectX-Graphics-Samples"
set "SRC=%SAMPLES%\Samples\Desktop\D3D12Raytracing\src"
set "APP=%SRC%\D3D12RaytracingHelloWorld"
if not exist "%APP%\D3D12RaytracingHelloWorld.cpp" (
  echo Sample not found at "%APP%".
  echo Pass the samples root, e.g.:  build_sample.bat C:\DW\DirectX-Graphics-Samples
  exit /b 1
)

set "DXC=%DXC_SDK_DIR%"
if "%DXC%"=="" set "DXC=C:\DW\DXC"
set "AGILITY=%AGILITY_SDK_DIR%"
if "%AGILITY%"=="" set "AGILITY=C:\DW\microsoft.direct3d.d3d12.1.619.5"
if not exist "%AGILITY%\build\native\include\d3d12.h" (
  echo Agility d3d12.h not found under "%AGILITY%\build\native\include". & exit /b 1
)

call "%~dp0setup_msvc.bat" || exit /b 1

set "OUT=%~dp0sampletest"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\CompiledShaders" mkdir "%OUT%\CompiledShaders"

rem --- shader: the .vcxproj FxCompile step, done by hand ---------------------
rem   ShaderType=Library, ShaderModel=6.3, VariableName=g_pRaytracing
"%DXC%\bin\x64\dxc.exe" -T lib_6_3 -Vn g_pRaytracing ^
   -Fh "%OUT%\CompiledShaders\Raytracing.hlsl.h" "%APP%\Raytracing.hlsl"
if errorlevel 1 exit /b 1

rem --- app -------------------------------------------------------------------
cl /nologo /EHsc /std:c++17 /O2 /DUNICODE /D_UNICODE ^
   /I "%AGILITY%\build\native\include" /I "%APP%" /I "%SRC%" /I "%OUT%" ^
   /Fo:"%OUT%\\" ^
   "%APP%\D3D12RaytracingHelloWorld.cpp" "%APP%\DXSample.cpp" ^
   "%APP%\DeviceResources.cpp" "%APP%\Main.cpp" "%APP%\Win32Application.cpp" ^
   "%APP%\stdafx.cpp" ^
   /Fe:"%OUT%\D3D12RaytracingHelloWorld.exe" ^
   /link d3d12.lib dxgi.lib dxguid.lib user32.lib gdi32.lib shell32.lib ole32.lib ^
   /SUBSYSTEM:WINDOWS /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem --- runtime bits next to the exe ------------------------------------------
rem Agility runtime: the sample sets D3D12SDKPath = ".\D3D12\".
if not exist "%OUT%\D3D12" mkdir "%OUT%\D3D12"
copy /y "%AGILITY%\build\native\bin\x64\D3D12Core.dll"      "%OUT%\D3D12\" >nul
copy /y "%AGILITY%\build\native\bin\x64\d3d12SDKLayers.dll" "%OUT%\D3D12\" >nul

echo.
echo Built sampletest\D3D12RaytracingHelloWorld.exe (no proxy yet).
echo To test the proxy:  copy /y d3d12.dll sampletest\  then run the exe.
