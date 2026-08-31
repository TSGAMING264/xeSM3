# xeSM3

**Spider-Man 3 PC Loose Resource Mod Loader**  
**Created by TSGAMING264**

[**Download Latest xeSM3 Release**](https://github.com/TSGAMING264/xeSM3/releases/latest)

xeSM3 allows resources in the Windows PC version of *Spider-Man 3* to be replaced with loose files stored inside a `Mods` directory. Mods can replace supported resources without rebuilding the game's original PCPACK archives.

The initial public release is **xeSM3 v0.1.0** for **Spider-Man 3 PC / Windows x86**.

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
- `Mods\`

Your game directory should look like this:

```text
Spider-Man 3\
├── Game.exe
├── dbghelp.dll
├── xeSM3.dll
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

## Making Mods

Loose resources use the original archive scope in their directory path:

```text
Mods\<Mod Name>\<PACK>\<APKF>\<resource>
```

Character example:

```text
Mods\TESTMOD\
└── CH_SPIDERMAN\
    └── _O0069.0xCFB154CD.T36.apkf\
        ├── 0xAC92103D.ch_spiderman000.mesh
        └── 0xAC92103E.ch_spiderman001.mesh
```

Texture example:

```text
Mods\TESTMOD\
└── SPIDERMANLOGO\
    └── _O0001.0x348E72F4.T36.apkf\
        └── 0xDEF62318.i_loading_screen_bkg.tex
```

The v0.1.0 release includes `TESTMOD` as an opt-in example. It must still be enabled in `mods.config.ini` before it will load.

See [docs/MAKING_MODS.md](docs/MAKING_MODS.md) for path rules, hashes, catalogs, and example workflows.

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
- Public Release builds intentionally compile startup diagnostic popups out.
- Official examples are opt-in and should only be enabled when the user chooses to test them.
- The repository contains source and documentation; tested binary packages belong on GitHub Releases.

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for the supplied public release history.

## Project Status & Community Help

xeSM3 is a major milestone for Spider-Man 3 PC modding, but there is still more work to do.

The Blender side of the project is not TSGAMING264's strongest area. A lot of effort was put into getting the Blender tools this far, and community help is strongly welcomed for improving:

- Mesh importing and exporting
- Skeleton workflows
- Materials
- Texture workflows
- Blender compatibility
- Reliability

This is an invitation to contribute and build on a working foundation—not a warning that the project is unusable.

> I have a lot of faith in the Spider-Man modding community, and I hope xeSM3 gives people a strong foundation to build from.

The post-processing fix is **not included** in xeSM3 v0.1.0. It is still planned and is coming later; it is not a blocker for this release.

**xeSM3 v0.1.0 is the beginning, not the end. ❤️**

## ❤️ Special Thanks

**Kirbystealer** — Huge shoutout to the legend behind exWoS. exWoS was a massive inspiration for xeSM3 and demonstrated what was possible with loose-resource mod loading. Without Kirbystealer's work, xeSM3 would not exist.

**AkyrosXD** — Huge shoutout for recreating the Spider-Man 3 Debug Menu. That debug menu became one of the most important tools throughout the research and reverse-engineering process that ultimately led to xeSM3.

**Josuke777** — Huge shoutout for making a serious attempt at recreating an exWoS-style system for Spider-Man 3 and for sharing notes, discoveries, methods, and information about model importing. That work provided valuable information during xeSM3's development.

**Devryx** — Huge shoutout for creating the Web of Shadows Blender Kit. That project provided an important reference and foundation while the Spider-Man 3 Blender tools were being developed.

**Haruse** — Huge shoutout for the Web of Shadows Blender tooling, including the texture conversion workflow and material/model research that provided useful reference material while adapting similar ideas for Spider-Man 3.

**Tmprogamer** — Huge thanks for beta testing xeSM3 and putting the loader through real-world testing.

**ArchiverOfTriviality** — Huge thanks for beta testing xeSM3 and helping verify stability, compatibility, and mod-loading behavior.

**Arc** — Huge shoutout for helping throughout xeSM3 testing and release QA, including pushing important compatibility tests such as the 4GB Patch, non-Spider-Man model testing, and DXVK / Vulkan compatibility.

xeSM3 was built on years of experimentation, reverse engineering, testing, and knowledge shared throughout the Spider-Man modding community.

Thank you to everyone who helped make this possible.

## Source Code and Releases

The frozen Visual Studio source is organized under [`src/xeSM3/`](src/xeSM3/). Blender tool sources are organized separately under [`tools/`](tools/).

Compiled DLLs and packaged release ZIPs are intentionally not committed to the normal source tree. Official tested packages are distributed through [GitHub Releases](https://github.com/TSGAMING264/xeSM3/releases/latest).

This is an unofficial fan-made modding project. No original Spider-Man 3 PCPACK archives or `Game.exe` are included.
