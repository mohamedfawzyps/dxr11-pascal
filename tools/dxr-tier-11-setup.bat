@echo off
rem Runs dxr-tier-11-setup.ps1 without making anyone think about execution policy.
rem Bypass applies to this one invocation only and changes no machine setting.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0dxr-tier-11-setup.ps1"
