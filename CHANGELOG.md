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
