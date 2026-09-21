@echo off
rem ---------------------------------------------------------------------------
rem Phase 5 recon material.
rem
rem Extracts the four Phase 2 shaders from phase2\raytest.cpp, then compiles and
rem disassembles each one with the SAME profiles raytest.cpp uses, cs_6_5 for
rem the RayQuery side and lib_6_3 for the TraceRay side. Phase 2 proved these
rem four equivalent in pairs on real hardware, so they are the specification for
rem what the rewriter has to consume and produce.
rem
rem Everything under phase5\shaders and phase5\dxil is generated and gitignored.
rem Findings live in docs\phase5-dxil-recon.md.
rem ---------------------------------------------------------------------------
setlocal

set DXC=C:\DW\DXC\bin\x64\dxc.exe
if not exist "%DXC%" (
    echo ERROR: dxc.exe not found at %DXC%
    exit /b 1
)

python "%~dp0tools\extract_shaders.py"
if errorlevel 1 exit /b 1

if not exist "%~dp0phase5\dxil" mkdir "%~dp0phase5\dxil"

echo.
call :build rayquery_opaque cs_6_5  -E main
if errorlevel 1 exit /b 1
call :build rayquery_alpha  cs_6_5  -E main
if errorlevel 1 exit /b 1
call :build traceray_opaque lib_6_3
if errorlevel 1 exit /b 1
call :build traceray_alpha  lib_6_3
if errorlevel 1 exit /b 1

echo.
echo Disassembly in phase5\dxil\*.ll, containers in phase5\dxil\*.dxil
echo Findings in docs\phase5-dxil-recon.md
echo.
echo   grep rayQuery phase5\dxil\rayquery_alpha.ll     the opcodes we consume
echo   grep AnyHit   phase5\dxil\traceray_alpha.ll     the shape we must emit
exit /b 0

:build
rem %1 basename, %2 target profile, %3.. extra args (entry point)
"%DXC%" -T %2 %3 %4 -Fc "%~dp0phase5\dxil\%1.ll" -Fo "%~dp0phase5\dxil\%1.dxil" ^
        "%~dp0phase5\shaders\%1.hlsl"
if errorlevel 1 (
    echo FAILED: %1 ^(%2^)
    exit /b 1
)
echo   ok  %1 ^(%2^)
exit /b 0
