# xeSM3 v0.1.0 — Release Candidate Source

**Spider-Man 3 PC Loose Resource Mod Loader**  
**Created by TSGAMING264**

This release-candidate source is derived from the proven `XESM3_V0_1_6_1_X86_CONFIG_FIX_SOURCE.zip` baseline.
The loader core is intentionally frozen. Release cleanup must not redesign or refactor resource-loading behavior.

## Build

Open `XESM3.sln` in Visual Studio 2022, select:

- Configuration: `Release`
- Platform: `x86`

Then build the solution. A successful build must produce both:

```text
bin\x86\Release\dbghelp.dll
bin\x86\Release\xeSM3.dll
```

The `Release|x86` configuration intentionally does **not** define `XESM3_BOOT_DIAGNOSTIC`, so the old startup diagnostic MessageBox popups are compiled out of the public build. Debug builds retain diagnostics for troubleshooting.

## Frozen core

The proven loader supports the release-tested loose resource routes:

- MESH
- MAT
- TEX
- ANIM
- SKEL
- ASKL

Do not remove or rename hook calls merely because some historical function names contain words such as `Diagnostic`, `Probe`, or `Trace`. Several of those names are historical and are part of the proven loader path.

## Configuration

`Mods\mods.config.ini` exposes only:

```ini
0   = Disabled
100 = Enabled
```

No generalized numeric priority system should be added.

## Official examples

The `Examples` folder contains the two release examples:

1. Spider-Man dual character MESH example.
2. SPIDERMANLOGO loading-screen TEX example.

They live outside `Mods` so they are not loaded automatically.

## Codex

Before making any final build changes, read **`CODEX_RELEASE_INSTRUCTIONS.md`** in full and follow it as the authoritative release task specification.
