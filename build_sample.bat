@echo off
setlocal
rem Build a Microsoft DXR 1.0 sample with cl.exe, straight from the
rem DirectX-Graphics-Samples checkout, with no NuGet restore and WITHOUT
rem modifying that repo. Output goes to .\sampletest\<SampleName>\.
rem
rem   build_sample.bat                                    HelloWorld, release
rem   build_sample.bat D3D12RaytracingSimpleLighting      SimpleLighting, release
rem   build_sample.bat D3D12RaytracingSimpleLighting debug
rem
rem Samples root: %DXSAMPLES_DIR%, default C:\DW\DirectX-Graphics-Samples.
rem
rem Why not msbuild: the upstream .vcxproj needs NuGet packages
rem (Microsoft.Direct3D.D3D12 + WinPixEventRuntime) and nuget.exe is not
rem installed here. These samples have no PIX code, only the msbuild import, so
rem PIX is skipped and the Agility SDK already on this machine is used instead.
rem
rem These apps exist to validate the proxy against real windowed DXR 1.0 apps:
rem swapchain, Present, continuous frames, Agility SDK runtime.
rem
rem Known to work for D3D12RaytracingHelloWorld and D3D12RaytracingSimpleLighting,
rem which share a file layout, an FxCompile setup (Raytracing.hlsl, lib_6_3,
rem g_pRaytracing) and include dirs. See tools\patch_sample.ps1 for the upstream
rem uninitialised-member bug that both of them carry.

set "NAME=%~1"
if "%NAME%"=="" set "NAME=D3D12RaytracingHelloWorld"

set "SAMPLES=%DXSAMPLES_DIR%"
if "%SAMPLES%"=="" set "SAMPLES=C:\DW\DirectX-Graphics-Samples"
set "SRC=%SAMPLES%\Samples\Desktop\D3D12Raytracing\src"
set "APP=%SRC%\%NAME%"
if not exist "%APP%\%NAME%.cpp" (
  echo Sample not found: "%APP%\%NAME%.cpp"
  echo Available samples under "%SRC%":
  dir /b /ad "%SRC%" 2>nul
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

set "OUT=%~dp0sampletest\%NAME%"
if not exist "%~dp0sampletest" mkdir "%~dp0sampletest"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\CompiledShaders" mkdir "%OUT%\CompiledShaders"
if not exist "%OUT%\patched" mkdir "%OUT%\patched"
rem cl's /Fo: with a trailing backslash needs the directory to already exist.
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if not exist "%OUT%\objd" mkdir "%OUT%\objd"

rem --- work around the upstream uninitialised-member bug ----------------------
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\patch_sample.ps1" ^
  -Source "%APP%\%NAME%.cpp" -Dest "%OUT%\patched\%NAME%.cpp"
if errorlevel 1 exit /b 1

rem --- shader: the .vcxproj FxCompile step, done by hand -----------------------
rem   ShaderType=Library, ShaderModel=6.3, VariableName=g_p%(Filename)
"%DXC%\bin\x64\dxc.exe" -T lib_6_3 -Vn g_pRaytracing ^
   -Fh "%OUT%\CompiledShaders\Raytracing.hlsl.h" "%APP%\Raytracing.hlsl"
if errorlevel 1 exit /b 1

rem --- app --------------------------------------------------------------------
rem The patched .cpp is compiled in place of the upstream one; every other
rem translation unit comes straight from the samples checkout. Include order
rem matches the upstream .vcxproj: "..\" (for d3dx12.h) and the generated-shader
rem dir, plus the Agility SDK headers that its .props would have added.
set "CFLAGS=/nologo /EHsc /std:c++17 /DUNICODE /D_UNICODE /DWIN32 /D_WINDOWS"
set "INCS=/I "%AGILITY%\build\native\include" /I "%APP%" /I "%SRC%" /I "%OUT%""
set "SOURCES="%OUT%\patched\%NAME%.cpp" "%APP%\DXSample.cpp" "%APP%\DeviceResources.cpp" "%APP%\Main.cpp" "%APP%\Win32Application.cpp" "%APP%\stdafx.cpp""
set "LIBS=d3d12.lib dxgi.lib dxguid.lib user32.lib gdi32.lib shell32.lib ole32.lib"

if /i "%~2"=="debug" goto :debug

cl %CFLAGS% /O2 /DNDEBUG %INCS% /Fo:"%OUT%\obj\\" %SOURCES% ^
   /Fe:"%OUT%\%NAME%.exe" ^
   /link %LIBS% /SUBSYSTEM:WINDOWS /INCREMENTAL:NO
if errorlevel 1 exit /b 1
set "BUILT=%NAME%.exe"
goto :runtime

:debug
rem /D_DEBUG turns on the sample's own debug-layer path, which reports to
rem OutputDebugString. Note it also enables SetBreakOnSeverity(ERROR), so the app
rem dies at the first debug-layer error; the messages arrive before it does.
cl %CFLAGS% /Zi /MDd /D_DEBUG %INCS% /Fo:"%OUT%\objd\\" /Fd:"%OUT%\objd\vc.pdb" %SOURCES% ^
   /Fe:"%OUT%\%NAME%_dbg.exe" ^
   /link %LIBS% /SUBSYSTEM:WINDOWS /INCREMENTAL:NO /DEBUG
if errorlevel 1 exit /b 1
set "BUILT=%NAME%_dbg.exe"

:runtime
rem Agility runtime next to the exe: the samples set D3D12SDKPath = ".\D3D12\".
if not exist "%OUT%\D3D12" mkdir "%OUT%\D3D12"
copy /y "%AGILITY%\build\native\bin\x64\D3D12Core.dll"      "%OUT%\D3D12\" >nul
copy /y "%AGILITY%\build\native\bin\x64\d3d12SDKLayers.dll" "%OUT%\D3D12\" >nul

echo.
echo Built sampletest\%NAME%\%BUILT%
echo Proxy test:  copy /y d3d12.dll sampletest\%NAME%\   then run it.
