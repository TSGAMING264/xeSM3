# xeSM3 v0.1.0 — Release Cleanup Summary

This release-candidate copy was prepared from the proven v0.1.6.1 source baseline.

## Changes intentionally made

- Public branding changed to **xeSM3 v0.1.0**.
- Added safe source comments: **Created by TSGAMING264**.
- Public payload output name changed to `xeSM3.dll`; bootstrap remains `dbghelp.dll`.
- `DbgHelpProxy.cpp` now loads `xeSM3.dll` beside itself.
- Removed `XESM3_BOOT_DIAGNOSTIC` from **Release** project definitions only, so public Release startup MessageBox diagnostics are compiled out.
- Debug builds retain diagnostics for troubleshooting.
- Removed bundled developer mesh-test folders/scripts from the release-candidate copy.
- Replaced the shipped `Mods\mods.config.ini` with the final public 0/100 configuration and credits.
- Updated only the default-config **text block** in `XESM3ResourceRedirector.cpp`; resource-loader algorithms were not changed.
- Added official MESH and TEX examples outside `Mods` so they are opt-in.
- Updated x86 build/release scripts for the public filename/version.
- Added `CODEX_RELEASE_INSTRUCTIONS.md`, `VALIDATE_RELEASE_CANDIDATE.py`, and `FINAL_SMOKE_TEST.md`.

## Frozen behavior not changed

- MESH / MAT / TEX / ANIM / SKEL / ASKL routes
- Hook addresses / offsets / hashes
- Detour attach sequence
- PACK + APKF strict resource scoping
- Shadow resource behavior / ownership / lifetime
- Game.exe compatibility gate
- `dbghelp.dll` proxy architecture
- 0/100 mod-toggle parser behavior
- RaimiHook independence / `d3d9.dll` independence

Do not perform additional cleanup inside the frozen loader merely for style.
