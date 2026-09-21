# xeSM3 WoS-style WRAP External MAT Test

This source tree is derived from the xeSM3 v0.1.0 final-release performance-cleanup baseline.
It intentionally changes only the WRAP/NativeMESH material-reference path needed to reproduce the Web of Shadows Blender workflow.

## Reference behavior verified

The implementation was checked against:

- KirbyStealer/WebOfShadowsTools v0.2.1 `Wrapper.wrapResourceFile` contract.
- WoS BlenderToolkit 1.0.2 exporter behavior.
- User-supplied WoS `.wrap.mesh` exports, including a joined/skinned mesh and an Electro tutorial mod.

A Blender-generated WoS MESH leaves each external material pointer field at zero. The WRAP external patch table supplies a 16-byte entry containing:

1. resource FourCC (`MAT\0`)
2. material filename hash
3. expected index (`0xFFFFFFFF` for Blender exports)
4. relative pointer to the MESH material field

The joined WoS reference additionally carries an external `SKEL` entry and a `NAME` entry. xeSM3 continues preserving its stock top-level MESH identity/SKEL pointer; this test change is deliberately scoped to external MAT resolution only.

## xeSM3 change

`TryUnwrapLooseResource` now captures WRAP external-patch metadata while it normalizes internal pointers. NativeMESH matches an external `MAT` entry by the flattened material-field offset. It then resolves the stock SM3 material by TYPE+HASH through SM3's existing game resource resolver, using the canonical resource name from `Mods/filelist.apkf.txt`.

This means the SM3 Blender exporter can move toward the same behavior as WoS:

- Blender material identity = real SM3 MAT hash, e.g. `0xE52A3DF4`.
- MESH material field can remain zero in the standalone WRAP.
- WRAP carries `MAT + hash + target` instead of a hard-coded archive-local serialized value such as `0x00000614`.
- The material's normal SM3 TEX references remain authoritative, so loose TEX overrides continue to use their real TEX hashes.

## Backward compatibility

Raw `.mesh` files and older WRAP files with no external MAT entries continue through the existing serialized-material fallback. Internal WRAP pointer normalization is unchanged. TEX/MAT/ANIM/SKEL/ASKL and PostFX code paths are otherwise untouched.

## First runtime test

Use a Blender-exported SM3 WRAP MESH that contains an external MAT patch to an existing SM3 material hash. For the known full-body canvas experiment, the intended identity is:

- MAT `0xE52A3DF4` = `ch_spidermanspider`
- TEX `0x8E661B33` = `ch_spiderman_spider`

The next Blender addon build should emit the MAT hash externally instead of writing the old `0x614` serialized value.

This package is a test source branch, not a new public version number.


## Runtime footprint note
The external-reference name cache is intentionally MAT-only. It does not duplicate names for TEX, MESH, CVX, ANIM, SKEL, or other catalog resource types.


## MSVC C2712 guard
This test branch adds **no new `__try/__except` blocks**. External MAT resolution uses the same direct stock-resource-resolver call pattern already used by xeSM3 NativeMAT. This keeps the baseline SEH surface unchanged and avoids C2712 object-unwinding conflicts.


## C2712 actual fix
The first C2712 attempts only counted SEH blocks. The actual conflict was `std::string` locals added to the pre-existing `ResolveChSpidermanPlayerMaterialPointer()` SEH function. Those strings now live in `ResolveWrapExternalMaterialForMeshSection()`, which contains no SEH. The fallback function keeps its baseline SEH but no new RAII locals. Run `VALIDATE_C2712_ACTUAL_FIX.py` before building.
