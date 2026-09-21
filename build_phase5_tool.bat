@echo off
rem Builds the Phase 5 round-trip / assemble tool into phase5out\.
setlocal
set DXC=C:\DW\DXC
call "%~dp0setup_msvc.bat" || exit /b 1
if not exist "%~dp0phase5out" mkdir "%~dp0phase5out"
cl /nologo /EHsc /std:c++17 /O2 /W4 /I "%DXC%\inc" ^
   /Fo:"%~dp0phase5out\\" ^
   phase5\dxilrt.cpp /Fe:"%~dp0phase5out\dxilrt.exe" /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
copy /y "%DXC%\bin\x64\dxcompiler.dll" "%~dp0phase5out\" >nul
copy /y "%DXC%\bin\x64\dxil.dll"       "%~dp0phase5out\" >nul
echo.
echo Built phase5out\dxilrt.exe
