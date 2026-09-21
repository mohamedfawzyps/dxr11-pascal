@echo off
rem Put the MSVC x64 toolchain (cl.exe) on PATH for the calling script.
rem No-op if cl.exe is already available, so launching from an
rem "x64 Native Tools Command Prompt for VS" still works as before.
rem Must be invoked with CALL so the environment changes persist.

where cl >nul 2>nul && exit /b 0

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [setup_msvc] vswhere.exe not found.
  echo Install "Build Tools for Visual Studio" with the
  echo "Desktop development with C++" workload, or run this from an
  echo "x64 Native Tools Command Prompt for VS".
  exit /b 1
)

set "VSPATH="
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo [setup_msvc] No VC++ toolchain found. Install the
  echo "Desktop development with C++" workload.
  exit /b 1
)

set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo [setup_msvc] vcvars64.bat not found under "%VSPATH%".
  exit /b 1
)
echo [setup_msvc] activating MSVC x64 from "%VSPATH%"
call "%VCVARS%" >nul
where cl >nul 2>nul || (
  echo [setup_msvc] vcvars64.bat ran but cl.exe is still not on PATH.
  exit /b 1
)
exit /b 0
