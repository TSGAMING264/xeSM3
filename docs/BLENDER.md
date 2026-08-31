# Blender and Model Tools

The repository includes source for two separate Blender tools. They support Spider-Man 3 modding workflows, but they do not make every model, skeleton, material, or Blender-version combination universal or automatic.

The supplied extension manifests declare **Blender 4.5.0** as the minimum version.

## SM3 Blender Toolkit v1.1.7

Source directory:

```text
tools/SM3-Blender-Toolkit/
```

The toolkit provides:

- SM3 MESH import
- SM3 SKEL import
- SM3 MESH export
- Skeleton export support
- WoS-style index-based vertex-group renaming
- Original-piece and joined-model workflows
- Object/section-aware export behavior
- Preservation of the imported target resource hash and basename

The v1.1.7 workflow uses the imported SKEL as the authoritative game-index-to-bone-name map. It is designed to avoid character-specific rename tables and to support non-Spider-Man skeletons as well as Spider-Man targets.

Read [`tools/SM3-Blender-Toolkit/README.md`](../tools/SM3-Blender-Toolkit/README.md) before working with imported pieces, joined models, vertex groups, or the proven 48-bone palette limit.

## SM3 Material Combiner Toolkit v1.2.0

Source directory:

```text
tools/SM3-Material-Combiner/
```

This is a separate material and texture research add-on. It provides:

- REAL SM3 material database access
- Serialized-reference to REAL MAT hash resolution
- MAT-to-TEX reporting
- Legacy material-name upgrades
- Duplicate REAL MAT cleanup
- Resolved DDS texture loading
- DDS/TEX conversion workflows
- Slot-safe atlas creation and UV remapping
- Preservation of SM3 material-slot count and polygon material indices in the slot-safe workflow

Version 1.2.0 collects atlas inputs by object and material slot so repeated material hashes remain independently traceable. See the included README and v1.2.0 release notes for the exact behavior and current test layout.

## Installing a Tool in Blender

For a packaged tool release:

1. Keep the individual tool's install ZIP intact.
2. In Blender, open **Edit → Preferences → Add-ons / Extensions → Install from Disk**.
3. Select the ZIP for that tool, not the ZIP for this entire GitHub repository.
4. Enable the installed extension.

For source development, keep the tool directory structure intact, with `__init__.py` and `blender_manifest.toml` at the extension root. Consult Blender's extension-development documentation for the correct development installation path for your Blender version.

The toolkit panels are declared at:

- **SM3 Blender Toolkit:** File Import/Export and the 3D View N-panel under **SM3 Tools**
- **SM3 Material Combiner Toolkit:** 3D View N-panel under **SM3 Materials**

## Workflow Notes

- Preserve original target files before exporting replacements.
- Test one model or material change at a time.
- Keep PACK, APKF, hash, and output filename information with the exported resource.
- A successful Blender export still needs the correct xeSM3 mod path.
- Material previews do not by themselves prove that every in-game material route is correct.
- Keep source meshes, skeletons, textures, and Blender projects outside the Git repository unless they are original distributable project assets.

## Project Status & Community Help

xeSM3 is a major milestone for Spider-Man 3 PC modding, but there is still more work to do.

The Blender side of the project is not TSGAMING264's strongest area. A lot of effort went into getting these tools this far, and community contributions are strongly welcomed for improving:

- Mesh importing and exporting
- Skeleton workflows
- Materials
- Texture workflows
- Blender compatibility
- Reliability

This is an invitation to improve a useful foundation, not a warning that the project is unusable.

> I have a lot of faith in the Spider-Man modding community, and I hope xeSM3 gives people a strong foundation to build from.

The post-processing fix is not included in xeSM3 v0.1.0. It remains planned for later and is not a release blocker.

**xeSM3 v0.1.0 is the beginning, not the end. ❤️**
