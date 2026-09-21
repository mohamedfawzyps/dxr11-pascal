@echo off
setlocal
rem Build the Phase 1 signing test with MSVC (no CMake needed).
rem Run from an "x64 Native Tools Command Prompt for VS".
rem
rem   build.bat C:\path\to\dxc        (dir containing inc\dxcapi.h)
rem or set DXC_SDK_DIR first, then: build.bat

set "DXC=%~1"
if "%DXC%"=="" set "DXC=%DXC_SDK_DIR%"
if "%DXC%"=="" set "DXC=C:\DW\DXC"
if not exist "%DXC%\inc\dxcapi.h" (
  echo dxcapi.h not found under "%DXC%\inc".
  echo Pass the DXC SDK path, e.g.:  build.bat C:\path\to\dxc
  echo (the folder that contains inc\dxcapi.h and bin\x64\dxcompiler.dll^)
  exit /b 1
)
call "%~dp0setup_msvc.bat" || exit /b 1

cl /nologo /EHsc /std:c++17 /I "%DXC%\inc" src\signtest.cpp /Fe:signtest.exe /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1

rem Copy the runtime DLLs next to the exe if we can find them.
if exist "%DXC%\bin\x64\dxcompiler.dll" copy /y "%DXC%\bin\x64\dxcompiler.dll" . >nul
if exist "%DXC%\bin\x64\dxil.dll"       copy /y "%DXC%\bin\x64\dxil.dll" . >nul

echo.
echo Built signtest.exe. Run:  signtest.exe   (or --target=header^|bytecode^|both)
if not exist dxil.dll echo WARNING: dxil.dll not found - copy a signed one next to the exe.
