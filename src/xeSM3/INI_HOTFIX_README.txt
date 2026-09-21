xeSM3 v0.1.0 FINAL CONFIGURATION
Created by TSGAMING264

MODS
----
Only this mod config is read:
  <Game.exe>\Mods\mods.config.ini

Public values:
  0   = Disabled
  100 = Enabled

No arbitrary numeric priority system is supported.

POSTFX
------
PostFX settings are stored in:
  <Game.exe>\xeSM3.ini

Public values are also strict 0 / 100.

Default final configuration:
  PostProcessFix=100
  PostProcessRetailXbox=0
  PostProcessDebugXbox=100

The Retail Xbox and Debug Xbox PostFX routes are alternative renderer profiles.
Enable only one Xbox profile at a time.
Keep PostProcessFix=100 when using either Xbox profile.

WRAP
----
Standalone WRAP resources are supported through the normal loose-resource paths.
Examples:
  0xHASH.name.wrap.mesh
  0xHASH.name.wrap.tex
  0xHASH.name.wrap.mat

ALT-TAB / RESET
---------------
The final source contains the V10.5.72 D3D9 Reset fix. xeSM3 releases its own device resources before Reset and rebuilds them only after a successful Reset.
