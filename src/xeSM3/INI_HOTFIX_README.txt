xeSM3 v0.1.0 - INI HOTFIX

ROOT CAUSES FIXED IN THIS SOURCE
===============================

1. DUPLICATE INI ENTRY BUG
   Old behavior:
       Example Mod=100
       Example Mod=0

   Both lines were kept as separate package records.
   The first 100 record was still scanned, so the mod appeared "stuck on".

   New behavior:
   LAST assignment wins, like a normal INI.

2. LEGACY ROOT CONFIG RESURRECTION
   Old behavior:
   If Mods\mods.config.ini was missing, xeSM3 could copy an old
   <Game.exe>\mods.config.ini into Mods\mods.config.ini.

   New behavior:
   ONLY <Game.exe>\Mods\mods.config.ini is authoritative.
   A root-level legacy config is ignored.

3. UTF-8 BOM
   Some Windows editors save a BOM before [EnabledMods].
   The parser now strips it safely.

PUBLIC TOGGLE CONTRACT
======================

0   = Disabled
100 = Enabled

No priority system is added.

CRITICAL CLEAN TEST
===================

Before replacing the public download:

A) Close Game.exe completely.
B) Temporarily remove/rename d3d9.dll so RaimiHook cannot act as a second loader.
C) Delete/rename any root-level:
       <Game.exe>\mods.config.ini
D) Use ONLY:
       <Game.exe>\Mods\mods.config.ini

To determine whether the "example" is actually baked into a modified game pack:

1. Temporarily remove dbghelp.dll and xeSM3.dll.
2. Launch the game with NO xeSM3 loader at all.
3. If the example change STILL appears, it is not coming from xeSM3.
   Restore clean stock game PCPACK resources before continuing.
4. If the example disappears, rebuild and test this INI hotfix source.
