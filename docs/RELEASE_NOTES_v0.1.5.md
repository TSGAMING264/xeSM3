# xeSM3 v0.1.5

xeSM3 v0.1.5 improves the post-processing fix and restored bloom. Special thanks to Bread and ArchiverOfTriviality (Arc) for their contributions toward these improvements. This release also moves xeSM3 to a single `dbghelp.dll`.

The release package contains the new `dbghelp.dll`, its matching `xeSM3.ini`, the `Mods` directory, examples, and an install guide. It does not require a separate `xeSM3.dll` or an xeSM3 `winmm.dll`. WRAP support was introduced in v0.1.0 and remains documented in the repository.

## Upgrade from an older xeSM3 version

Close the game and remove the previous xeSM3-owned `dbghelp.dll` and separate `xeSM3.dll` beside `Game.exe`. Remove `winmm.dll` only if it came from an earlier xeSM3 test build; do not remove one belonging to another mod. Install the new `dbghelp.dll`, `xeSM3.ini`, and `Mods` folder beside `Game.exe`. Leaving old xeSM3 DLLs installed can cause conflicts. RaimiHook's `d3d9.dll` is not an obsolete xeSM3 DLL and should not be removed for this migration. Back up any existing mod configuration before replacing `Mods`.

## Known issue

RaimiHook's debug menu currently requires keyboard input. Controller input does not operate the menu properly; use the keyboard as the workaround.
