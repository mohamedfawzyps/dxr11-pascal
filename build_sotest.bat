@echo off
rem Builds the offline state object probe into phase5out\.
rem
rem It exists because chasing a driver crash through a game launch costs
rem minutes per question. sotest asks one question in a second, on one shader,
rem with nothing else running. Feed it the pair the shim dumps:
rem
rem   sotest lowered_006.out.dxil lowered_006.rs.bin
rem   sotest lowered_006.out.dxil lowered_006.rs.bin warp
setlocal
call "%~dp0setup_msvc.bat" || exit /b 1
if not exist "%~dp0phase5out" mkdir "%~dp0phase5out"
cl /nologo /EHsc /std:c++17 /O2 /W4 ^
   /Fo:"%~dp0phase5out\\" ^
   phase5\sotest.cpp /Fe:"%~dp0phase5out\sotest.exe" /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
echo.
echo Built phase5out\sotest.exe
