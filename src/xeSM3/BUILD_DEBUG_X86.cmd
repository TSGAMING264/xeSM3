@echo off
setlocal
cd /d "%~dp0"
echo ============================================================
echo   xeSM3 v0.1.0 - DUAL-DLL x86 DEBUG BUILD
echo   Builds BOTH: dbghelp.dll bootstrap + xeSM3.dll payload
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
msbuild XESM3.sln /m /t:Build /p:Configuration=Debug /p:Platform=x86
if errorlevel 1 (
  echo BUILD FAILED
  pause
  exit /b 1
)
if not exist "%~dp0bin\x86\Debug\dbghelp.dll" (
  echo ERROR: dbghelp.dll missing from Debug output.
  pause
  exit /b 1
)
if not exist "%~dp0bin\x86\Debug\xeSM3.dll" (
  echo ERROR: xeSM3.dll missing from Debug output.
  pause
  exit /b 1
)
echo.
echo BUILD PASS - BOTH x86 DLLs
echo Bootstrap: %~dp0bin\x86\Debug\dbghelp.dll
echo Payload:   %~dp0bin\x86\Debug\xeSM3.dll
echo.
pause
