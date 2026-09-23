@echo off
rem Builds the C++ rewriter port and its CLI into phase5out\dxrw.exe.
rem
rem The CLI's "rewrite" mode needs DXC beside it, exactly as the proxy will:
rem the rewriter works on .ll text while D3D12 hands over a container, so
rem dxcompiler.dll converts both ways and dxil.dll signs the result.
setlocal

set DXC=C:\DW\DXC
call "%~dp0setup_msvc.bat" || exit /b 1
if not exist "%~dp0phase5out" mkdir "%~dp0phase5out"

cl /nologo /EHsc /std:c++17 /O2 /W4 /I "%DXC%\inc" ^
   /Fo:"%~dp0phase5out\\" ^
   phase5\dxrw.cpp proxy\rewriter\ll_model.cpp proxy\rewriter\rq_analyze.cpp ^
   proxy\rewriter\rq_lower.cpp proxy\rewriter\dxc_host.cpp proxy\dllinfo.cpp ^
   /Fe:"%~dp0phase5out\dxrw.exe" /link version.lib /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem Loaded by full path from beside the exe, never by bare name: an
rem application may already have its own dxcompiler.dll of another version.
copy /y "%DXC%\bin\x64\dxcompiler.dll" "%~dp0phase5out\" >nul
copy /y "%DXC%\bin\x64\dxil.dll"       "%~dp0phase5out\" >nul

echo.
echo Built phase5out\dxrw.exe
