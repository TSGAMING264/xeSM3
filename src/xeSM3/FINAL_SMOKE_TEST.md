# xeSM3 v0.1.0 — Final Smoke Test

After Codex/Visual Studio produces the Release binaries, test the exact binaries intended for release.

- [ ] Game.exe boots normally.
- [ ] No xeSM3 startup diagnostic MessageBox appears.
- [ ] `dbghelp.dll` loads `xeSM3.dll`.
- [ ] One Spider-Man MESH example works.
- [ ] Loading-screen TEX example works.
- [ ] `0` disables a mod.
- [ ] `100` enables a mod.
- [ ] MESH / MAT / TEX / ANIM / SKEL / ASKL proven routes remain functional.
- [ ] Optional RaimiHook setup still works when its `d3d9.dll` is used.
- [ ] DXVK/Vulkan setup still works when DXVK x32 `d3d9.dll` is used instead.
- [ ] No XESM3/xeSM3 resource log is created.

If these pass, package the exact tested DLLs — do not rebuild again before release.
