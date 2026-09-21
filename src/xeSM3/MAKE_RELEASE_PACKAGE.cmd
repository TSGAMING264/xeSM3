@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "OUT=%~dp0RELEASE\xeSM3 v0.1.0"

if not exist "bin\x86\Release\xeSM3.dll" (
  echo ERROR: Build Release x86 first. xeSM3.dll is missing.
  exit /b 1
)
if not exist "bin\x86\Release\dbghelp.dll" (
  echo ERROR: Build Release x86 first. dbghelp.dll is missing.
  exit /b 1
)
if not exist "xeSM3.ini" (
  echo ERROR: xeSM3.ini is missing.
  exit /b 1
)

if exist "%OUT%" rmdir /S /Q "%OUT%"
mkdir "%OUT%\Mods"
mkdir "%OUT%\Examples"

copy /Y "bin\x86\Release\xeSM3.dll" "%OUT%\xeSM3.dll" >nul
copy /Y "bin\x86\Release\dbghelp.dll" "%OUT%\dbghelp.dll" >nul
copy /Y "xeSM3.ini" "%OUT%\xeSM3.ini" >nul
copy /Y "Mods\mods.config.ini" "%OUT%\Mods\mods.config.ini" >nul
copy /Y "Mods\filelist.txt" "%OUT%\Mods\filelist.txt" >nul
copy /Y "Mods\filelist.apkf.txt" "%OUT%\Mods\filelist.apkf.txt" >nul
copy /Y "Mods\filelist.apkf.paths.txt" "%OUT%\Mods\filelist.apkf.paths.txt" >nul
xcopy /E /I /Y "Examples\*" "%OUT%\Examples\" >nul

(
 echo xeSM3 v0.1.0
 echo Spider-Man 3 PC Loose Resource Mod Loader
 echo Created by TSGAMING264
 echo.
 echo INSTALL:
 echo Copy dbghelp.dll, xeSM3.dll, xeSM3.ini, and the Mods folder into the Spider-Man 3 game directory beside Game.exe.
 echo.
 echo MODS:
 echo Add mod folders under Mods\ and list them under [EnabledMods] in Mods\mods.config.ini.
 echo 0 = Disabled
 echo 100 = Enabled
 echo No other numeric priority values are supported.
 echo.
 echo XBOX POSTFX:
 echo xeSM3.ini controls PostFX. Public values are also 0 or 100 only.
 echo Default: Debug Xbox route enabled.
 echo Route priority: Debug Xbox ^> Retail Xbox ^> PostProcessFix ^> Retail PC.
 echo.
 echo WRAP:
 echo Standalone .wrap.mesh/.wrap.tex/.wrap.mat/.wrap.anim/.wrap.skel/.wrap.askl inputs are supported through the native loose-resource processing routes.
 echo Malformed WRAP input fails closed.
 echo.
 echo ALT-TAB / DEVICE RESET:
 echo This release includes the tested V10.5.72 D3D9 Reset fix for Alt-Tab/device-loss recovery.
 echo.
 echo EXAMPLES:
 echo The Examples folder is documentation only. Copy an example mod folder into Mods\ and enable it intentionally if you want to test it.
 echo.
 echo RAIMIHOOK:
 echo The original RaimiHook debug menu can coexist with xeSM3 through its own d3d9.dll.
 echo.
 echo DXVK / VULKAN:
 echo DXVK also uses d3d9.dll, so use the DXVK x32 d3d9.dll instead of RaimiHook's d3d9.dll when testing Vulkan.
) > "%OUT%\INSTALL.txt"

echo RELEASE PACKAGE READY:
echo %OUT%
exit /b 0
