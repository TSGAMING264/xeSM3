@echo off
setlocal EnableExtensions DisableDelayedExpansion
if "%~1"=="" (
    set "XESM3_SINGLE_OUTPUT=%~dp0build"
) else (
    set "XESM3_SINGLE_OUTPUT=%~f1"
)
set "XESM3_SINGLE_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%XESM3_SINGLE_VSWHERE%" (
    echo Visual Studio 2022 C++ build tools and a Windows SDK are required.
    exit /b 1
)
set "XESM3_SINGLE_VSROOT="
for /f "usebackq delims=" %%I in (`"%XESM3_SINGLE_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "XESM3_SINGLE_VSROOT=%%I"
if not defined XESM3_SINGLE_VSROOT (
    echo The MSVC C++ x86 build tools were not found.
    exit /b 1
)
"%XESM3_SINGLE_VSROOT%\MSBuild\Current\Bin\MSBuild.exe" "%~dp0xeSM3_0.1.5\XESM3.vcxproj" /m /t:Build /p:Configuration=Release /p:Platform=Win32 /p:XESM3Proxy=dbghelp "/p:OutDir=%XESM3_SINGLE_OUTPUT%\output/" "/p:IntDir=%XESM3_SINGLE_OUTPUT%\obj/" "/p:TLogLocation=%XESM3_SINGLE_OUTPUT%\obj\tlog/" /nologo
if errorlevel 1 exit /b 1
if not exist "%XESM3_SINGLE_OUTPUT%\output\dbghelp.dll" exit /b 1
if not exist "%XESM3_SINGLE_OUTPUT%\output\xeSM3.ini" (
    copy /y "%~dp0xeSM3_0.1.5\xeSM3.ini" "%XESM3_SINGLE_OUTPUT%\output\xeSM3.ini" >nul
    if errorlevel 1 exit /b 1
)
echo Built: "%XESM3_SINGLE_OUTPUT%\output\dbghelp.dll"
echo Configuration: "%XESM3_SINGLE_OUTPUT%\output\xeSM3.ini"
echo No installation or deployment performed.
exit /b 0
