@echo off
setlocal
rem Build the Microsoft DXR 1.0 sample "D3D12RaytracingHelloWorld" with cl.exe,
rem straight from the DirectX-Graphics-Samples checkout, with no NuGet restore
rem and WITHOUT modifying that repo. Output goes to .\sampletest\.
rem
rem   build_sample.bat [path\to\DirectX-Graphics-Samples] [debug]
rem
rem Why not msbuild: the upstream .vcxproj needs NuGet packages
rem (Microsoft.Direct3D.D3D12 1.619.0 + WinPixEventRuntime) and nuget.exe is not
rem installed here. The sample has no PIX code, only the msbuild import, so PIX
rem is skipped and the Agility SDK already on this machine is used instead.
rem
rem This app exists to validate the Phase 3a proxy against a real windowed DXR
rem 1.0 app: swapchain, Present, continuous frames, Agility SDK runtime.
rem
rem UPSTREAM BUG, patched below: D3D12RaytracingHelloWorld::m_descriptorsAllocated
rem (D3D12RaytracingHelloWorld.h:65) is never initialised by the constructor. It
rem is only zeroed in ReleaseDeviceDependentResources(), which does not run
rem before the first CreateDeviceDependentResources(). AllocateDescriptor()
rem therefore reads an uninitialised member on first init and hands
rem CreateUnorderedAccessView a garbage descriptor index:
rem   D3D12 ERROR: ID3D12Device::CreateUnorderedAccessView: Specified CPU
rem   descriptor handle ... does not refer to a location in a descriptor heap.
rem   [ EXECUTION ERROR #646: INVALID_DESCRIPTOR_HANDLE ]
rem followed by a GPU hang and an access violation. The sample object is a local
rem in WinMain, so whether this bites depends on stack contents; it reproduces
rem reliably with this toolchain. We patch a generated copy of the .cpp rather
rem than editing the samples checkout.

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
if not exist "%OUT%\patched" mkdir "%OUT%\patched"

rem --- patch the uninitialised member into a generated copy --------------------
powershell -NoProfile -Command ^
  "$src = Get-Content -Raw '%APP%\D3D12RaytracingHelloWorld.cpp';" ^
  "$anchor = '    m_raytracingOutputResourceUAVDescriptorHeapIndex(UINT_MAX)';" ^
  "if ($src -notmatch [regex]::Escape($anchor)) { Write-Error 'patch anchor not found'; exit 1 };" ^
  "$fix = '    m_descriptorsAllocated(0),' + [char]13 + [char]10 + $anchor;" ^
  "[IO.File]::WriteAllText('%OUT%\patched\D3D12RaytracingHelloWorld.cpp', $src.Replace($anchor, $fix));" ^
  "Write-Host '[patch] m_descriptorsAllocated(0) added to constructor init list'"
if errorlevel 1 exit /b 1

rem --- shader: the .vcxproj FxCompile step, done by hand -----------------------
rem   ShaderType=Library, ShaderModel=6.3, VariableName=g_pRaytracing
"%DXC%\bin\x64\dxc.exe" -T lib_6_3 -Vn g_pRaytracing ^
   -Fh "%OUT%\CompiledShaders\Raytracing.hlsl.h" "%APP%\Raytracing.hlsl"
if errorlevel 1 exit /b 1

rem --- app --------------------------------------------------------------------
rem The patched .cpp is compiled in place of the upstream one; every other
rem translation unit comes straight from the samples checkout.
rem Include order matches the upstream .vcxproj: "..\" (for d3dx12.h) and the
rem generated-shader dir, plus the Agility SDK headers from its .props.
set "CFLAGS=/nologo /EHsc /std:c++17 /DUNICODE /D_UNICODE /DWIN32 /D_WINDOWS"
set "INCS=/I "%AGILITY%\build\native\include" /I "%APP%" /I "%SRC%" /I "%OUT%""
set "SOURCES="%OUT%\patched\D3D12RaytracingHelloWorld.cpp" "%APP%\DXSample.cpp" "%APP%\DeviceResources.cpp" "%APP%\Main.cpp" "%APP%\Win32Application.cpp" "%APP%\stdafx.cpp""
set "LIBS=d3d12.lib dxgi.lib dxguid.lib user32.lib gdi32.lib shell32.lib ole32.lib"

if /i "%~2"=="debug" goto :debug

cl %CFLAGS% /O2 /DNDEBUG %INCS% /Fo:"%OUT%\obj\\" %SOURCES% ^
   /Fe:"%OUT%\D3D12RaytracingHelloWorld.exe" ^
   /link %LIBS% /SUBSYSTEM:WINDOWS /INCREMENTAL:NO
if errorlevel 1 exit /b 1
set "BUILT=D3D12RaytracingHelloWorld.exe"
goto :runtime

:debug
rem /D_DEBUG turns on the sample's own debug-layer path. Note it also enables
rem SetBreakOnSeverity(ERROR), so the app dies at the first debug-layer error.
cl %CFLAGS% /Zi /MDd /D_DEBUG %INCS% /Fo:"%OUT%\objd\\" /Fd:"%OUT%\objd\vc.pdb" %SOURCES% ^
   /Fe:"%OUT%\HelloWorldDbg.exe" ^
   /link %LIBS% /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /DEBUG
if errorlevel 1 exit /b 1
set "BUILT=HelloWorldDbg.exe"

:runtime
rem Agility runtime next to the exe: the sample sets D3D12SDKPath = ".\D3D12\".
if not exist "%OUT%\D3D12" mkdir "%OUT%\D3D12"
copy /y "%AGILITY%\build\native\bin\x64\D3D12Core.dll"      "%OUT%\D3D12\" >nul
copy /y "%AGILITY%\build\native\bin\x64\d3d12SDKLayers.dll" "%OUT%\D3D12\" >nul

echo.
echo Built sampletest\%BUILT%
echo Proxy test:  copy /y d3d12.dll sampletest\  then run it.
