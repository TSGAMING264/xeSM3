# xeSM3 v0.1.0 — Final Release Source

**Spider-Man 3 PC Loose Resource Mod Loader**  
**Created by TSGAMING264**

This is the final v0.1.0 source package. It keeps the proven public loader layout from the original v0.1.0 release and adds the completed Xbox-style PostFX restoration, WRAP-format loose-resource support, strict `0/100` configuration, and the V10.5.72 Alt-Tab/D3D9 device-reset fix.

## Public release layout

The generated user-facing package intentionally stays simple:

```text
xeSM3 v0.1.0\
├─ dbghelp.dll
├─ xeSM3.dll
├─ xeSM3.ini
├─ Mods\
│  ├─ mods.config.ini
│  ├─ filelist.txt
│  ├─ filelist.apkf.txt
│  └─ filelist.apkf.paths.txt
├─ Examples\
└─ INSTALL.txt
```

No RaimiHook research logs, PostFX research logs, source files, PDBs, or developer-only assets are copied into the public runtime package.

## Build the final release

Run:

```text
BUILD_FINAL_RELEASE_X86.cmd
```

The script builds the two x86 DLLs and assembles the final runtime folder/ZIP under `RELEASE\`.

Expected binaries:

```text
bin\x86\Release\dbghelp.dll
bin\x86\Release\xeSM3.dll
```

## Configuration — 0 / 100 only

`Mods\mods.config.ini` uses only:

```ini
0   = Disabled
100 = Enabled
```

No generalized numeric priority system is used.

`xeSM3.ini` uses the same public toggle convention for PostFX:

```ini
[PostProcessing]
PostProcessFix=100
PostProcessRetailXbox=0
PostProcessDebugXbox=100
```

When more than one PostFX route is enabled, route priority is:

```text
Debug Xbox > Retail Xbox > PostProcessFix > Retail PC
```

The shipped default is the qualified Debug Xbox route.

## Supported loose-resource routes

The proven loader routes remain:

- MESH
- MAT
- TEX
- ANIM
- SKEL
- ASKL

WRAP resources can now feed those same native processing routes, including names such as:

```text
0xHASH.name.wrap.mesh
0xHASH.name.wrap.tex
0xHASH.name.wrap.mat
0xHASH.name.wrap.anim
0xHASH.name.wrap.skel
0xHASH.name.wrap.askl
```

Malformed or unsupported WRAP input fails closed.

## Xbox-style PostFX restoration

The final public source includes the qualified production path:

- adaptive Debug Xbox bloom controller
- native weighted bloom stage
- same-present RESZ/INTZ depth recovery
- dormant F18 depth-aware final combine
- ImageZoom / camera-motion ZBlur
- xeSM3-owned GodRay final draw
- one-for-one suppression of the duplicate stock GodRay draw
- fail-closed stock fallback
- D3D9 Reset handling for Alt-Tab/device-loss recovery

The V10.5.72 runtime test passed repeated Alt-Tab/return without the prior crash.

## Final rule

The renderer/resource-loader implementation is frozen from the runtime-passed V10.5.72 source. Final packaging changes must not redesign the loader or PostFX pipeline.
