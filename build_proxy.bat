@echo off
setlocal
rem Build the Phase 3a forwarding-only proxy d3d12.dll with MSVC.
rem Run from an "x64 Native Tools Command Prompt for VS".
rem
rem   build_proxy.bat
rem
rem Output: d3d12.dll. Copy it into the folder of a DXR 1.0 app's exe and run
rem the app; Windows loads this proxy, which forwards to the real system
rem d3d12.dll. Check %TEMP%\dxr11_proxy.log for the interception line.

call "%~dp0setup_msvc.bat" || exit /b 1

cl /nologo /EHsc /std:c++17 /LD proxy\d3d12_proxy.cpp /Fe:d3d12.dll ^
   /link /DEF:proxy\d3d12_proxy.def
if errorlevel 1 exit /b 1

echo.
echo Built d3d12.dll (forwarding proxy). Copy it next to a DXR 1.0 sample exe,
echo run the sample, then check %%TEMP%%\dxr11_proxy.log.
