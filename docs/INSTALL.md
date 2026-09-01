# Installing xeSM3

xeSM3 v0.1.1 is the INI and packaging hotfix for the Windows x86 xeSM3 v0.1.0 loader for *Spider-Man 3*.

## What You Need

- An installed copy of the original Spider-Man 3 PC game
- The tested xeSM3 v0.1.1 release package from [GitHub Releases](https://github.com/TSGAMING264/xeSM3/releases/latest)
- Permission to copy files into the directory containing `Game.exe`

Do not download random DLL mirrors. Use the release package published by TSGAMING264.

## Installation

1. Download and extract the latest xeSM3 release ZIP.
2. Open the folder containing the game's `Game.exe`.
3. Copy `dbghelp.dll`, `xeSM3.dll`, and the complete `Mods` directory beside `Game.exe`.
4. Keep the filenames unchanged.

The v0.1.1 ZIP is a drop-in package: these files and folders are at the ZIP root, not inside a second wrapper directory.

The result should be:

```text
Spider-Man 3\
├── Game.exe
├── dbghelp.dll
├── xeSM3.dll
└── Mods\
    ├── mods.config.ini
    ├── filelist.txt
    ├── filelist.apkf.txt
    └── filelist.apkf.paths.txt
```

Both DLLs are required. `dbghelp.dll` is the bootstrap, and it loads `xeSM3.dll` from the same directory.

## Enabling a Mod

Place the mod's directory under `Mods`, then add the exact directory name to `Mods\mods.config.ini`:

```ini
[EnabledMods]
My Mod=100
```

Only two values are supported:

```text
0   = Disabled
100 = Enabled
```

Other numeric values are not priorities and are not supported.

Only `Mods\mods.config.ini` is authoritative. A root-level `mods.config.ini` is ignored. If a mod name appears more than once, the last assignment wins.

## Confirming the Installation

Release builds intentionally do not display xeSM3 startup or success popups. The absence of a popup is expected.

No example mod is installed or enabled automatically. To test a reference example, copy it deliberately from the source `Examples` directory into a new folder under `Mods`, add that folder name with `100`, and then set it to `0` to verify last-assignment behavior.

## Compatibility Notes

xeSM3 has been tested with the original Spider-Man 3 PC game and with the following components or environments:

- 4GB Patch
- DxWnd
- DXVK / Vulkan
- Original/base RaimiHook

xeSM3 uses `dbghelp.dll`. Original/base RaimiHook normally uses `d3d9.dll`, while DXVK also uses `d3d9.dll`. Two different files cannot occupy the same `d3d9.dll` path simultaneously, so RaimiHook's `d3d9.dll` and DXVK's `d3d9.dll` cannot simply be installed together under the same filename.

This compatibility list does not claim that every combination of third-party tools has been tested.

## Troubleshooting

### The game does not start

- Confirm that you installed the Windows x86 xeSM3 release.
- Confirm that both DLLs are directly beside the real `Game.exe`, not inside a second nested folder.
- Restore a clean game executable if you are testing an unsupported executable build.
- Remove unrelated proxy DLL experiments and test again from a known game setup.

Do not rename `dbghelp.dll` or `xeSM3.dll`, and do not replace the bootstrap with a `d3d9.dll` version.

### The game starts, but the mod does not load

- Confirm the mod directory name exactly matches its entry in `[EnabledMods]`.
- Confirm the entry is set to `100`.
- Confirm you edited only `Mods\mods.config.ini`; delete or ignore any stale root-level `mods.config.ini`.
- Confirm the path includes the correct PACK and APKF scope.
- Confirm the resource filename and hash are exact.
- Confirm the resource type is one of MESH, MAT, TEX, ANIM, SKEL, or ASKL.
- Check the included catalog files when identifying PACK, APKF, and resource paths.

### No startup message appears

That is normal. Diagnostic message boxes are compiled out of the public Release build.

### RaimiHook and DXVK conflict

The conflict is between their two different `d3d9.dll` files, not xeSM3's `dbghelp.dll`. Choose the `d3d9.dll` setup appropriate for the test you are performing. Do not assume an untested chain-loading combination is supported.

## Updating

When a future xeSM3 version is released, follow its release notes. Back up your `Mods` directory and `mods.config.ini` before replacing files.

## Uninstalling

Remove `dbghelp.dll` and `xeSM3.dll` from beside `Game.exe`. Remove the `Mods` directory only if you also want to remove your installed mods and configuration.
