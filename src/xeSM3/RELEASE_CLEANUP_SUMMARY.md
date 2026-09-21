# xeSM3 v0.1.0 — Final Release Summary

The final package keeps the original public v0.1.0 shape and updates the internals only where required.

## Included

- `dbghelp.dll` bootstrap
- `xeSM3.dll` payload
- `xeSM3.ini` for PostFX route selection
- `Mods\mods.config.ini` with strict `0/100` enable values
- three loader catalogs
- official examples outside `Mods`
- native loose MESH/MAT/TEX/ANIM/SKEL/ASKL support
- WRAP decode/flatten/fixup path feeding the proven native adapters
- Debug/Retail Xbox PostFX route selection
- V10.5.72 D3D9 Reset/Alt-Tab recovery

## Public defaults

```ini
[PostProcessing]
PostProcessFix=100
PostProcessRetailXbox=0
PostProcessDebugXbox=100
```

The Retail Xbox and Debug Xbox PostFX routes are alternative renderer profiles. Enable only one Xbox profile at a time. Keep `PostProcessFix=100` when using either Xbox profile.

```ini
[EnabledMods]
; My Mod=100
```

Only `0` and `100` are accepted as public toggle values.

## Intentionally not shipped in the runtime ZIP

- source code
- PDB files
- RaimiHook debug/research logs
- persistent PostFX research log
- persistent resource-request log
- developer test mods
- automatic example activation
- extra dependency folders

## Release performance cleanup

The final source also keeps research-only hot-path instrumentation out of the public Release build:

- `RenderMeshSkinningProbeHook` detour is installed only in diagnostic builds
- NativeANIM runtime diagnostic detours are installed only in diagnostic builds
- the 8,192-entry PostFX research event ring is not allocated or populated in `NDEBUG` Release builds

The production NativeANIM redirector, PostFX validation counters/fail-closed state, V10.5.72 Reset slot 16, DrawPrimitive slot 81, WRAP processing, and all proven loose-resource routes remain enabled and unchanged.
