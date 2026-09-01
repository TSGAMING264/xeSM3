@echo off
setlocal
cd /d "%~dp0"
set "OUT=%~dp0RELEASE\xeSM3 v0.1.1"
set "ZIP=%~dp0RELEASE\xeSM3 v0.1.1.zip"

for %%I in ("%OUT%") do set "OUT=%%~fI"
for %%I in ("%ZIP%") do set "ZIP=%%~fI"
if /I not "%OUT%"=="%~dp0RELEASE\xeSM3 v0.1.1" (
  echo ERROR: Refusing unexpected release output path: %OUT%
  exit /b 1
)
if /I not "%ZIP%"=="%~dp0RELEASE\xeSM3 v0.1.1.zip" (
  echo ERROR: Refusing unexpected release ZIP path: %ZIP%
  exit /b 1
)

if not exist "bin\x86\Release\xeSM3.dll" (
  echo ERROR: Build Release x86 first. xeSM3.dll is missing.
  pause
  exit /b 1
)
if not exist "bin\x86\Release\dbghelp.dll" (
  echo ERROR: Build Release x86 first. dbghelp.dll is missing.
  pause
  exit /b 1
)

if exist "%OUT%" rmdir /S /Q "%OUT%"
if exist "%ZIP%" del /Q "%ZIP%"
mkdir "%OUT%\Mods"
mkdir "%OUT%\Examples"

copy /Y "bin\x86\Release\xeSM3.dll" "%OUT%\xeSM3.dll" >nul
copy /Y "bin\x86\Release\dbghelp.dll" "%OUT%\dbghelp.dll" >nul
copy /Y "Mods\mods.config.ini" "%OUT%\Mods\mods.config.ini" >nul
copy /Y "Mods\filelist.txt" "%OUT%\Mods\filelist.txt" >nul
copy /Y "Mods\filelist.apkf.txt" "%OUT%\Mods\filelist.apkf.txt" >nul
copy /Y "Mods\filelist.apkf.paths.txt" "%OUT%\Mods\filelist.apkf.paths.txt" >nul
"%SystemRoot%\System32\xcopy.exe" /E /I /Y "Examples\*" "%OUT%\Examples\" >nul

(
 echo xeSM3 v0.1.1 - INI Hotfix
 echo Spider-Man 3 PC Loose Resource Mod Loader
 echo Created by TSGAMING264
 echo.
 echo INSTALL:
 echo Copy dbghelp.dll, xeSM3.dll, and the Mods folder into the Spider-Man 3 game directory beside Game.exe.
 echo.
 echo CONFIG:
 echo ONLY Mods\mods.config.ini is authoritative. A root-level mods.config.ini is ignored.
 echo Add mod folders under Mods\ and list them under [EnabledMods] in Mods\mods.config.ini.
 echo 0 = Disabled
 echo 100 = Enabled
 echo Last duplicate assignment wins.
 echo.
 echo EXAMPLES:
 echo The Examples folder is reference material only. No example mod is installed or enabled automatically.
 echo.
 echo RAIMIHOOK:
 echo The original RaimiHook debug menu can coexist with xeSM3 through its own d3d9.dll.
 echo.
 echo DXVK / VULKAN:
 echo DXVK also uses d3d9.dll, so use the DXVK x32 d3d9.dll instead of RaimiHook's d3d9.dll when testing Vulkan.
)>"%OUT%\INSTALL.txt"

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -LiteralPath '%OUT%\dbghelp.dll','%OUT%\xeSM3.dll','%OUT%\Mods','%OUT%\Examples','%OUT%\INSTALL.txt' -DestinationPath '%ZIP%' -CompressionLevel Optimal"
if errorlevel 1 (
  echo ERROR: Release ZIP creation failed.
  pause
  exit /b 1
)

echo.
echo RELEASE PACKAGE READY:
echo %ZIP%
echo.
echo ZIP ROOT CONTENTS:
echo   dbghelp.dll
echo   xeSM3.dll
echo   Mods\mods.config.ini
echo   Mods\filelist.txt
echo   Mods\filelist.apkf.txt
echo   Mods\filelist.apkf.paths.txt
echo   Examples\...
echo   INSTALL.txt
pause
