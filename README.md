# xeSM3

**Spider-Man 3 PC Mod Loader and Native Renderer Restoration Project**
**Created by TSGAMING264**

[**Download Latest xeSM3 Release**](https://github.com/TSGAMING264/xeSM3/releases/latest)

xeSM3 is a Spider-Man 3 PC mod loader and native renderer restoration project created by TSGAMING264.

**v0.1.0** adds strict loose-resource mod loading, WRAP resource support, and restored Xbox-derived post-processing features using Spider-Man 3 PC's dormant/native rendering systems. It targets **Spider-Man 3 PC / Windows x86**.

## Features

- Loose-resource mod loading
- PACK/APKF-aware replacement paths
- Multiple mod folders
- Simple `0` / `100` enable system
- Spider-Man and non-Spider-Man character model support
- Large and high-resolution texture replacement
- Arbitrary-size ANIM replacement
- Skeleton replacement
- ASKL replacement
- Standalone WRAP input support
- Deterministic mod conflict handling
- Strict `0` / `100` enablement
- Adaptive Xbox-style bloom
- RESZ / INTZ sampleable-depth recovery
- Depth-aware F18 final combine
- ImageZoom / camera-motion ZBlur
- Native GodRay restoration with duplicate suppression
- D3D9 Alt-Tab/device-reset recovery

Supported loose resource types:

- MESH
- MAT
- TEX
- ANIM
- SKEL
- ASKL

## Installation

Download the latest packaged release from [GitHub Releases](https://github.com/TSGAMING264/xeSM3/releases/latest), then copy these items beside `Game.exe`:

- `dbghelp.dll`
- `xeSM3.dll`
- `xeSM3.ini`
- `Mods\`

Your game directory should look like this:

```text
Spider-Man 3\
├── Game.exe
├── dbghelp.dll
├── xeSM3.dll
├── xeSM3.ini
└── Mods\
```

See [docs/INSTALL.md](docs/INSTALL.md) for detailed installation and troubleshooting.

## Enabling Mods

Open `Mods\mods.config.ini` and list each installed mod under `[EnabledMods]`.

```ini
[EnabledMods]
My Mod=100
```

The only supported values are:

```text
0   = Disabled
100 = Enabled
```

The name on the left must exactly match the mod directory name inside `Mods`.

## PostFX Configuration

The Retail Xbox and Debug Xbox PostFX routes are alternative renderer profiles. Enable only one Xbox profile at a time. Keep `PostProcessFix=100` when using either Xbox profile.

**Debug Xbox (qualified and recommended):**

```ini
[PostProcessing]
PostProcessFix=100
PostProcessRetailXbox=0
PostProcessDebugXbox=100
```

**Retail Xbox:**

```ini
[PostProcessing]
PostProcessFix=100
PostProcessRetailXbox=100
PostProcessDebugXbox=0
```

`0` means disabled and `100` means enabled. Other public numeric values are not supported. Do not enable both Xbox profiles at the same time.

- `PostProcessDebugXbox`: the currently qualified and recommended Xbox-derived restoration route.
- `PostProcessRetailXbox`: the retained retail Xbox comparison/restoration route.
- `PostProcessFix`: master/native PostFX fix enablement; keep this set to `100` when using either Xbox profile.

The PostFX implementation restores and reuses native Spider-Man 3 PC renderer infrastructure. It is compiled directly into `xeSM3.dll`; it is not an external ReShade-style filter and does not require `d3d9.dll` for normal operation.

## Making Mods

Loose resources use the original archive scope in their directory path:

```text
Mods\<Mod Name>\<PACK>\<APKF>\<resource>
```

Character example:

```text
Mods\My Character Mod\
└── CH_SPIDERMAN\
    └── _O0069.0xCFB154CD.T36.apkf\
        ├── 0xAC92103D.ch_spiderman000.mesh
        └── 0xAC92103E.ch_spiderman001.mesh
```

Texture example:

```text
Mods\My Loading Screen\
└── SPIDERMANLOGO\
    └── _O0001.0x348E72F4.T36.apkf\
        └── 0xDEF62318.i_loading_screen_bkg.tex
```

No example mod is installed or enabled automatically. Reference resources remain under `src/xeSM3/Examples/` and must be copied into a deliberately created mod folder before they can load.

See [docs/MAKING_MODS.md](docs/MAKING_MODS.md) for path rules, hashes, catalogs, and example workflows.

## WRAP Resources

xeSM3 v0.1.0 introduces support for standalone Treyarch/WoS-style WRAP resources. WRAP files are unpacked and normalized in memory before being passed into xeSM3's existing native loose-resource routes.

Supported names include:

```text
0xHASH.name.wrap.mesh
0xHASH.name.wrap.tex
```

WRAP is an input container for the existing MESH, MAT, TEX, ANIM, SKEL, and ASKL routes. xeSM3 validates and unwraps it in memory, normalizes internal pointers, preserves external/global resource tokens, removes `.wrap` before native resource-name hashing, and passes the normalized resource into the existing strict loader path. Malformed WRAP input fails closed and leaves the stock resource in control. Source PCPACK/APKF archives are never rewritten.

## Compatibility

xeSM3 has been tested with:

- The original Spider-Man 3 PC game
- 4GB Patch
- DxWnd
- DXVK / Vulkan
- Original/base RaimiHook

xeSM3 uses `dbghelp.dll` for its bootstrap. Original/base RaimiHook normally uses `d3d9.dll`, and DXVK also uses `d3d9.dll`. RaimiHook's `d3d9.dll` and DXVK's `d3d9.dll` therefore cannot simply occupy the same filename at the same time.

These items have been tested as listed, but that does not claim that every possible combination of third-party tools is supported.

## Blender & Model Tools

This repository includes source for two Blender tools:

### SM3 Blender Toolkit v1.1.7

Source: [`tools/SM3-Blender-Toolkit/`](tools/SM3-Blender-Toolkit/)

Provides Spider-Man 3 MESH and SKEL import/export support, index-based vertex-group renaming, and object/section-aware export workflows.

### SM3 Material Combiner Toolkit v1.2.0

Source: [`tools/SM3-Material-Combiner/`](tools/SM3-Material-Combiner/)

Provides SM3 material-database tools, MAT-to-TEX research support, texture loading/conversion, material cleanup, and slot-safe atlas generation.

These tools are still an evolving part of the project. Read [docs/BLENDER.md](docs/BLENDER.md) and each tool's included README before using them.

## Documentation

- [Installation and troubleshooting](docs/INSTALL.md)
- [Making loose-resource mods](docs/MAKING_MODS.md)
- [Blender and model tools](docs/BLENDER.md)
- [Changelog](CHANGELOG.md)

## Notes

- xeSM3 v0.1.0 targets the Windows x86 release of Spider-Man 3 PC.
- xeSM3 v0.1.0 includes the tested V10.5.72 native PostFX integration and Alt-Tab/D3D9 Reset recovery.
- Only `Mods\mods.config.ini` is authoritative; a root-level `mods.config.ini` is ignored.
- Duplicate mod entries use last-assignment-wins behavior.
- Public Release builds intentionally compile startup diagnostic popups out.
- Official examples are opt-in and should only be enabled when the user chooses to test them.
- The repository contains source and documentation; tested binary packages belong on GitHub Releases.

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for the supplied public release history.

## Blender Tools & Community Help

The core xeSM3 loader and v0.1.0 renderer work are release-tested, but the Blender side of the project is still an area I would like to improve.

Community contributions are especially welcome for improving:

- MESH importing and exporting
- Skeleton workflows
- Materials
- Texture workflows
- Blender compatibility
- Exporter reliability
- Usability

This is an invitation to improve the Blender tooling, not a warning that the xeSM3 loader itself is unusable.

> I have a lot of faith in the Spider-Man modding community, and I hope people can continue improving the Blender side and build on the foundation xeSM3 provides.

The v0.1.0 PostFX restoration is included directly in `xeSM3.dll` and uses the tested narrow D3D9 Reset and DrawPrimitive hook pair.

**xeSM3 v0.1.0 is the beginning, not the end. ❤️**

## ❤️ Special Thanks

**Kirbystealer** — Huge shoutout to the legend behind exWoS. exWoS was a massive inspiration for xeSM3 and demonstrated what was possible with loose-resource mod loading. Without Kirbystealer's work, xeSM3 would not exist.

**AkyrosXD** — Huge shoutout for recreating the Spider-Man 3 Debug Menu. That debug menu became one of the most important tools throughout the research and reverse-engineering process that ultimately led to xeSM3.

**Josuke777** — Huge shoutout for making a serious attempt at recreating an exWoS-style system for Spider-Man 3 and for sharing notes, discoveries, methods, and information about model importing. That work provided valuable information during xeSM3's development.

**Devryx** — Huge shoutout for creating the Web of Shadows Blender Kit. That project provided an important reference and foundation while the Spider-Man 3 Blender tools were being developed.

**Haruse** — Huge shoutout for the Web of Shadows Blender tooling, including the texture conversion workflow and material/model research that provided useful reference material while adapting similar ideas for Spider-Man 3.

**Tmprogamer** — Huge thanks for beta testing xeSM3 and putting the loader through real-world testing.

**ArchiverOfTriviality** — Huge thanks for beta testing xeSM3 and helping throughout development and release QA, including verifying stability, compatibility, and mod-loading behavior, as well as helping identify the D3D9 device-reset / Alt-Tab issue and pushing important compatibility tests such as the 4GB Patch, non-Spider-Man model testing, and DXVK / Vulkan compatibility.

**Bread** — Huge thanks for SM3 IDA coding.

xeSM3 was built on years of experimentation, reverse engineering, testing, and knowledge shared throughout the Spider-Man modding community.

Thank you to everyone who helped make this possible.

## Source Code and Releases

The frozen Visual Studio source is organized under [`src/xeSM3/`](src/xeSM3/). Blender tool sources are organized separately under [`tools/`](tools/).

Compiled DLLs and packaged release ZIPs are intentionally not committed to the normal source tree. Official tested packages are distributed through [GitHub Releases](https://github.com/TSGAMING264/xeSM3/releases/latest).

This is an unofficial fan-made modding project. No original Spider-Man 3 PCPACK archives or `Game.exe` are included.
