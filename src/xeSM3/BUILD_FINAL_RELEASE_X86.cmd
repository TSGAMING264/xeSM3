@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo ============================================================
echo   xeSM3 v0.1.0 - FINAL RELEASE BUILD
echo   Old public layout + Xbox PostFX + WRAP + Reset fix
echo ============================================================
echo.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo ERROR: Visual Studio 2022 Build Tools not found.
  pause
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
  echo ERROR: MSBuild installation not found.
  pause
  exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars32.bat"
if errorlevel 1 (
  echo ERROR: Failed to initialize x86 compiler tools.
  pause
  exit /b 1
)

echo [1/5] Running source validators when Python is available...
where py >nul 2>nul
if not errorlevel 1 (
  py -3 VALIDATE_RELEASE_CANDIDATE.py
  if errorlevel 1 goto :fail
  py -3 VALIDATE_WRAP_SUPPORT.py
  if errorlevel 1 goto :fail
  py -3 VALIDATE_FINAL_RELEASE_SOURCE.py
  if errorlevel 1 goto :fail
) else (
  where python >nul 2>nul
  if not errorlevel 1 (
    python VALIDATE_RELEASE_CANDIDATE.py
    if errorlevel 1 goto :fail
    python VALIDATE_WRAP_SUPPORT.py
    if errorlevel 1 goto :fail
    python VALIDATE_FINAL_RELEASE_SOURCE.py
    if errorlevel 1 goto :fail
  ) else (
    echo WARNING: Python not found; pre-build validators skipped.
  )
)

echo [2/5] Building Release x86...
msbuild XESM3.sln /m /t:Rebuild /p:Configuration=Release /p:Platform=x86
if errorlevel 1 goto :fail

if not exist "bin\x86\Release\dbghelp.dll" (
  echo ERROR: dbghelp.dll missing from Release output.
  goto :fail
)
if not exist "bin\x86\Release\xeSM3.dll" (
  echo ERROR: xeSM3.dll missing from Release output.
  goto :fail
)

echo [3/5] Assembling old-style public runtime folder...
call "%~dp0MAKE_RELEASE_PACKAGE.cmd"
if errorlevel 1 goto :fail

echo [4/5] Creating final ZIP...
set "ZIP=%~dp0RELEASE\xeSM3_v0.1.0_FINAL.zip"
if exist "%ZIP%" del /Q "%ZIP%"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path '%~dp0RELEASE\xeSM3 v0.1.0\*' -DestinationPath '%ZIP%' -CompressionLevel Optimal -Force"
if errorlevel 1 goto :fail

echo [5/5] Writing SHA-256...
powershell -NoProfile -ExecutionPolicy Bypass -Command "$h=(Get-FileHash -Algorithm SHA256 '%ZIP%').Hash.ToLower(); Set-Content -Encoding ASCII '%~dp0RELEASE\xeSM3_v0.1.0_FINAL_SHA256.txt' ('SHA-256  '+$h+'  xeSM3_v0.1.0_FINAL.zip'); Write-Host $h"
if errorlevel 1 goto :fail

echo.
echo ============================================================
echo FINAL RELEASE READY
echo ============================================================
echo Folder: %~dp0RELEASE\xeSM3 v0.1.0
echo ZIP:    %ZIP%
echo SHA:    %~dp0RELEASE\xeSM3_v0.1.0_FINAL_SHA256.txt
echo.
echo Public ZIP contents remain the old xeSM3 layout, updated with xeSM3.ini for Xbox PostFX.
pause
exit /b 0

:fail
echo.
echo FINAL RELEASE BUILD FAILED
pause
exit /b 1
