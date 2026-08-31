# Making xeSM3 Mods

xeSM3 replaces supported Spider-Man 3 PC resources with loose files. It does not require rebuilding the original PCPACK archives.

## Required Directory Layout

Every loose resource must retain its original archive scope:

```text
Mods\<Mod Name>\<PACK>\<APKF>\<resource>
```

The parts are:

- `<Mod Name>`: the package name used in `mods.config.ini`
- `<PACK>`: the original PCPACK scope, such as `CH_SPIDERMAN`
- `<APKF>`: the exact APKF directory name, including its object number, hash, and type
- `<resource>`: the exact loose resource filename, including its resource hash and extension

PACK and APKF scoping matters. A matching filename placed under the wrong scope is not the same resource route.

## Enabling the Package

If the package directory is `Mods\My Mod`, configure it as:

```ini
[EnabledMods]
My Mod=100
```

The directory name and config name must match exactly.

```text
0   = Disabled
100 = Enabled
```

xeSM3 does not expose a generalized numeric mod-priority system. Values other than `0` and `100` are not supported.

## Character MESH Example

```text
Mods\TESTMOD\
└── CH_SPIDERMAN\
    └── _O0069.0xCFB154CD.T36.apkf\
        ├── 0xAC92103D.ch_spiderman000.mesh
        └── 0xAC92103E.ch_spiderman001.mesh
```

In this example:

- `CH_SPIDERMAN` is the PACK scope.
- `_O0069.0xCFB154CD.T36.apkf` is the APKF scope.
- `0xAC92103D` and `0xAC92103E` are resource hashes.
- The full resource names and `.mesh` extensions are preserved.

## Loading-Screen TEX Example

```text
Mods\TESTMOD\
└── SPIDERMANLOGO\
    └── _O0001.0x348E72F4.T36.apkf\
        └── 0xDEF62318.i_loading_screen_bkg.tex
```

Here, `SPIDERMANLOGO` is the PACK scope, `_O0001.0x348E72F4.T36.apkf` is the APKF scope, and `0xDEF62318` is the resource hash.

## Hashes, Names, and Catalogs

Do not invent or shorten resource filenames. Use the original hash, logical name, extension, PACK, and APKF path for the resource you intend to replace.

The release includes three reference catalogs under `Mods`:

- `filelist.txt`
- `filelist.apkf.txt`
- `filelist.apkf.paths.txt`

Use them to identify resource hashes and their PACK/APKF paths. These are reference catalogs; keep all three files installed.

## Supported Loose Resources

xeSM3 v0.1.0 supports these routes:

- MESH
- MAT
- TEX
- ANIM
- SKEL
- ASKL

The loose file must still be valid for the game and the target resource. xeSM3 redirects resource loading; it does not automatically repair malformed model, material, texture, animation, or skeleton data.

## Included TESTMOD and Examples

The v0.1.0 release includes `TESTMOD` for opt-in testing. It is not active until its matching config entry is set to `100`.

The source also contains official character MESH and loading-screen TEX examples. Keep examples opt-in rather than moving every example into `Mods` by default.

## Suggested Workflow

1. Find the target resource in the catalogs.
2. Record its exact PACK and APKF scope.
3. Prepare a valid replacement with the exact resource filename.
4. Create the full mod directory path.
5. Add the mod directory name to `mods.config.ini` with `100`.
6. Test one replacement at a time before combining changes.
7. Set the entry to `0` when comparing against the original game resource.

For MESH, SKEL, material, and texture workflows, see [BLENDER.md](BLENDER.md).
