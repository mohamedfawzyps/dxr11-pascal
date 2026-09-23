@echo off
setlocal
rem Build the proxy d3d12.dll with MSVC.
rem
rem   build_proxy.bat
rem
rem Output: d3d12.dll. Copy it into the folder of a DXR 1.0 app's exe and run
rem the app; Windows loads this proxy, which forwards to the real system
rem d3d12.dll and (since 3b) wraps the device. Check %TEMP%\dxr-tier-11-proxy.log.
rem
rem Headers: the Agility SDK ones if present, else the Windows SDK. The device
rem wrapper's method signatures were transcribed from the Agility 1.619.5
rem d3d12.h, so building against the same headers keeps them honest. The proxy
rem itself does not depend on Agility at runtime; it only needs the interface
rem declarations.

call "%~dp0setup_msvc.bat" || exit /b 1

set "AGILITY=%AGILITY_SDK_DIR%"
if "%AGILITY%"=="" set "AGILITY=C:\DW\microsoft.direct3d.d3d12.1.619.5"
set "INCS="
if exist "%AGILITY%\build\native\include\d3d12.h" (
  set "INCS=/I "%AGILITY%\build\native\include""
  echo [build_proxy] using Agility headers from "%AGILITY%"
) else (
  echo [build_proxy] Agility headers not found, using the Windows SDK
)

if not exist "%~dp0obj" mkdir "%~dp0obj"

rem Tail-jump thunks for the undocumented D3D12Core* layering exports. These
rem need no signature, which is the whole point; see proxy\d3d12_thunks.asm.
ml64 /nologo /c /Fo"%~dp0obj\d3d12_thunks.obj" proxy\d3d12_thunks.asm
if errorlevel 1 exit /b 1

rem The version resource, so the file says which build it is without being run.
rem The setup tool reads this; see proxy\d3d12_proxy.rc.
rc /nologo /I "%~dp0proxy" /fo "%~dp0obj\d3d12_proxy.res" proxy\d3d12_proxy.rc
if errorlevel 1 exit /b 1

cl /nologo /EHsc /std:c++17 /O2 /W4 %INCS% /I "C:\DW\DXC\inc" /LD ^
   /Fo:"%~dp0obj\\" ^
   proxy\d3d12_proxy.cpp proxy\d3d12_device.cpp proxy\state_object_cache.cpp proxy\queue_hook.cpp proxy\d3d12_command_list.cpp proxy\command_signature.cpp proxy\dxil_scan.cpp proxy\rq_pipeline.cpp proxy\as_tracker.cpp proxy\res_tracker.cpp proxy\config.cpp proxy\dllinfo.cpp proxy\proxy_log.cpp proxy\pso_stream.cpp proxy\shader_dump.cpp proxy\d3d12_pipeline_library.cpp ^
   proxy\rewriter\dxc_host.cpp proxy\rewriter\ll_model.cpp ^
   proxy\rewriter\rq_analyze.cpp proxy\rewriter\rq_lower.cpp proxy\rewriter\rq_bake.cpp ^
   /Fe:d3d12.dll ^
   /link version.lib "%~dp0obj\d3d12_thunks.obj" "%~dp0obj\d3d12_proxy.res" /DEF:proxy\d3d12_proxy.def /INCREMENTAL:NO
if errorlevel 1 exit /b 1

echo.
echo Built d3d12.dll. Copy it next to a DXR 1.0 sample exe, run the sample,
echo then check %%TEMP%%\dxr-tier-11-proxy.log.
echo Set DXR_TIER11_NOWRAP=1 to forward only, without wrapping the device.
