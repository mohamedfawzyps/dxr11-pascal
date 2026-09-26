@echo off
setlocal
rem Build the Tier 1.1 completion tests (tier11\*.cpp) with MSVC.
rem
rem   build_tier11.bat [path\to\agility]
rem
rem Output: gitest.exe in the REPOSITORY ROOT, beside the proxy d3d12.dll,
rem dxcompiler.dll and dxil.dll, so the hardware half runs through the shim
rem the way a game would. The WARP half runs through it too; the shim stands
rem aside on a device that already reports Tier 1.1.

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

set "OBJ=%~dp0obj\tier11"
if not exist "%OBJ%" mkdir "%OBJ%"

cl /nologo /EHsc /std:c++17 /O2 /W4 ^
   /I "%AGILITY%\build\native\include" /I "%DXC%\inc" ^
   /Fo:"%OBJ%\\" ^
   "%~dp0tier11\gitest.cpp" /Fe:"%~dp0gitest.exe" ^
   /link d3d12.lib dxgi.lib user32.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem The runtime's rules for a library's own subobjects (0.58.0).
cl /nologo /EHsc /std:c++17 /O2 /W4 ^
   /I "%AGILITY%\build\native\include" /I "%DXC%\inc" ^
   /Fo:"%OBJ%\\" ^
   "%~dp0tier11\libsubprobe.cpp" /Fe:"%~dp0libsubprobe.exe" ^
   /link d3d12.lib dxgi.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem How large a local root signature the driver takes (0.59.0).
cl /nologo /EHsc /std:c++17 /O2 /W4 ^
   /I "%AGILITY%\build\native\include" ^
   /Fo:"%OBJ%\\" ^
   "%~dp0tier11\lrsprobe.cpp" /Fe:"%~dp0lrsprobe.exe" ^
   /link d3d12.lib dxgi.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem What a deserialized acceleration structure contains, decoded for tools (0.61.0).
cl /nologo /EHsc /std:c++17 /O2 /W4 ^
   /I "%AGILITY%\build\native\include" ^
   /Fo:"%OBJ%\\" ^
   "%~dp0tier11\decodeprobe.cpp" /Fe:"%~dp0decodeprobe.exe" ^
   /link d3d12.lib dxgi.lib advapi32.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem Reserving descriptors at the end of an application's heap (0.63.0).
cl /nologo /EHsc /std:c++17 /O2 /W4 ^
   /I "%AGILITY%\build\native\include" ^
   /Fo:"%OBJ%\\" ^
   "%~dp0tier11\heapprobe.cpp" /Fe:"%~dp0heapprobe.exe" ^
   /link d3d12.lib dxgi.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1

echo.
echo Built gitest.exe, libsubprobe.exe, lrsprobe.exe, decodeprobe.exe and heapprobe.exe
echo Run:  gitest.exe [--sm66] [layout ...]
