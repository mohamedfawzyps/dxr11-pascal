@echo off
setlocal
rem Build the proxy dxgi.dll with MSVC.
rem
rem   build_dxgi.bat
rem
rem Output: dxgi.dll. Copy it next to d3d12.dll in the application's folder.
rem
rem It exists for one thing: Unreal Engine refuses ray tracing on Pascal by PCI
rem device id, after accepting the Tier 1.1 answer d3d12.dll gives it. The
rem device id comes through DXGI, so d3d12.dll cannot reach it. See
rem proxy\dxgi_spoof.h.
rem
rem Optional and separate on purpose: copying the file is the opt-in, and it
rem does nothing useful without d3d12.dll beside it.

call "%~dp0setup_msvc.bat" || exit /b 1

if not exist "%~dp0obj" mkdir "%~dp0obj"
rem Its own object directory: d3d12.dll and dxgi.dll share source files
rem (proxy_log.cpp, config.cpp) and would otherwise fight over the .obj.
if not exist "%~dp0obj\dxgi" mkdir "%~dp0obj\dxgi"

rem Tail-jump thunks for the seventeen exports with no public signature.
ml64 /nologo /c /Fo"%~dp0obj\dxgi_thunks.obj" proxy\dxgi_thunks.asm
if errorlevel 1 exit /b 1

rem The version resource, shared with d3d12.dll so both files say which build
rem they are without being run.
rc /nologo /I "%~dp0proxy" /fo "%~dp0obj\dxgi_proxy.res" proxy\dxgi_proxy.rc
if errorlevel 1 exit /b 1

cl /nologo /EHsc /std:c++17 /O2 /W4 /LD ^
   /Fo:"%~dp0obj\dxgi\\" ^
   proxy\dxgi_proxy.cpp proxy\dxgi_spoof.cpp proxy\proxy_log.cpp proxy\config.cpp ^
   /Fe:dxgi.dll ^
   /link "%~dp0obj\dxgi_thunks.obj" "%~dp0obj\dxgi_proxy.res" ^
   /DEF:proxy\dxgi_proxy.def /INCREMENTAL:NO
if errorlevel 1 exit /b 1

echo.
echo Built dxgi.dll. Copy it next to d3d12.dll in the application's folder.
echo Set DXR_TIER11_SPOOF=0, or spoof = 0 in dxr-tier-11.ini, to disable it.
