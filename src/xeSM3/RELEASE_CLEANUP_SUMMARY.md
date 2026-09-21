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
