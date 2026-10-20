# v0.1.5

- Improved the post-processing fix and restored bloom, using the supplied completed source and single-DLL build.
- Moved xeSM3 to one `dbghelp.dll`; the separate `xeSM3.dll` is no longer shipped.
- Special thanks to Bread and ArchiverOfTriviality (Arc) for their contributions toward these improvements.
- Upgrade: close the game; remove the previous xeSM3-owned `dbghelp.dll` and separate `xeSM3.dll`. Remove `winmm.dll` only if it came from an earlier xeSM3 test build. Install the new `dbghelp.dll`, `xeSM3.ini`, and `Mods` beside `Game.exe`. Do not remove another mod's `winmm.dll` or RaimiHook's `d3d9.dll`.
- Known issue: RaimiHook's debug menu currently requires keyboard input; controller input does not operate the menu properly.

# v0.1.0

Final tested first public release.

- MESH replacement
- MAT replacement
- TEX replacement
- ANIM replacement
- SKEL replacement
- ASKL replacement
- Standalone WRAP input support with fail-closed validation
- Internal WRAP pointer/fixup normalization and external/global token preservation
- Web of Shadows-style WRAP external-MAT resolution for NativeMESH exports
- Stock runtime mesh MAT-hash matching now precedes the global resolver fallback and reuses the exact live MAT pointer
- In-game verified with a Blender v0.5.1 zero-field WRAP mesh using MAT hash `0xE52A3DF4`
- Existing serialized-material fallback retained for raw and older WRAP MESH mods
- Scoped PCPACK/APKF resource replacement
- Multiple mod package support
- `0` / `100` config system
- Deterministic last-assignment-wins mod conflict handling
- V10.5.72 native PostFX integration
- Adaptive Xbox-style bloom and RESZ/INTZ sampleable depth
- F18 depth-aware final combine
- ImageZoom/camera-motion ZBlur
- Native GodRay restoration and stock duplicate suppression
- Alt-Tab/D3D9 Reset crash fix
- Removed Release mesh-skinning research probe overhead
- Removed Release NativeANIM diagnostic-hook overhead while retaining the production NativeANIM redirector
- Removed Release PostFX 8,192-entry research event-ring overhead
- Retained all production validation, fail-closed safety, resource-loader behavior, and renderer behavior
- Spider-Man model example
- Loading-screen TEX example
