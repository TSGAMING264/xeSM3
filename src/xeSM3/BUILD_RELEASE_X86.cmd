@echo off
setlocal
cd /d "%~dp0"
echo ============================================================
echo   xeSM3 v0.1.0 - DUAL-DLL x86 BUILD
echo   Builds: dbghelp.dll bootstrap + xeSM3.dll payload
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
msbuild XESM3.sln /m:1 /t:Rebuild /p:Configuration=Release /p:Platform=x86
if errorlevel 1 (
  echo BUILD FAILED
  pause
  exit /b 1
)
if not exist "%~dp0bin\x86\Release\dbghelp.dll" (
  echo ERROR: dbghelp.dll missing from Release output.
  pause
  exit /b 1
)
if not exist "%~dp0bin\x86\Release\xeSM3.dll" (
  echo ERROR: xeSM3.dll missing from Release output.
  pause
  exit /b 1
)
echo.
echo BUILD PASS - BOTH x86 DLLs
echo Bootstrap: %~dp0bin\x86\Release\dbghelp.dll
echo Payload:   %~dp0bin\x86\Release\xeSM3.dll
echo.
echo Copy BOTH DLLs and the Mods folder beside Game.exe.
pause
