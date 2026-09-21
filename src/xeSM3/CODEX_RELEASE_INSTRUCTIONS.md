# xeSM3 v0.1.0 — Final Release Instructions

The final public build must keep the original v0.1.0 release shape. Do not redesign the package.

## Frozen implementation

Do not refactor the runtime-passed V10.5.72 C++ implementation. It includes:

- proven loose-resource loader
- strict 0/100 mod configuration
- WRAP decode/fixup integration
- V10.5.71 qualified Xbox-style PostFX route
- V10.5.72 D3D9 Reset/Alt-Tab fix

## Required public runtime layout

```text
xeSM3 v0.1.0\
  dbghelp.dll
  xeSM3.dll
  xeSM3.ini
  Mods\
  Examples\
  INSTALL.txt
```

Do not ship source files, PDBs, research logs, debug reports, or development-only test folders in the runtime ZIP.

## Build

Use `BUILD_FINAL_RELEASE_X86.cmd`.

The public version remains `v0.1.0`. Internal PostFX milestone numbers are not public version numbers.

## WoS external-MAT test branch guard
Before building this test branch, run `VALIDATE_C2712_NO_NEW_SEH.py`. The WoS MAT feature must add no new `__try/__except` blocks beyond the proven baseline.
