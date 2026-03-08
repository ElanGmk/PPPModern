@echo off
setlocal

REM Invoke the PowerShell pre-commit hook on Windows.
set "HOOK_DIR=%~dp0"
set "PS_CMD=powershell.exe"

where %PS_CMD% >NUL 2>&1
if errorlevel 1 (
  echo powershell.exe not found; skipping formatting.
  endlocal & exit /b 0
)

%PS_CMD% -NoProfile -ExecutionPolicy Bypass -File "%HOOK_DIR%pre-commit.ps1"
set "CODE=%ERRORLEVEL%"
endlocal & exit /b %CODE%

