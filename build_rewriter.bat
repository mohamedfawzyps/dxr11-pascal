@echo off
rem Builds the C++ rewriter port and its CLI into phase5out\dxrw.exe.
setlocal
call "%~dp0setup_msvc.bat" || exit /b 1
if not exist "%~dp0phase5out" mkdir "%~dp0phase5out"
cl /nologo /EHsc /std:c++17 /O2 /W4 ^
   /Fo:"%~dp0phase5out\\" ^
   phase5\dxrw.cpp proxy\rewriter\ll_model.cpp proxy\rewriter\rq_analyze.cpp proxy\rewriter\rq_lower.cpp ^
   /Fe:"%~dp0phase5out\dxrw.exe" /link /INCREMENTAL:NO
if errorlevel 1 exit /b 1
echo.
echo Built phase5out\dxrw.exe
