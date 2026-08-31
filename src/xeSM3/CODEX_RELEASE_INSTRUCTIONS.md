# CODEX RELEASE INSTRUCTIONS — xeSM3 v0.1.0

## Mission

Prepare and build the final **xeSM3 v0.1.0** Windows x86 Release build from this source tree.
This tree comes from a proven runtime-tested loader. Treat the loader core as **FROZEN**.

Proven baseline archive: `XESM3_V0_1_6_1_X86_CONFIG_FIX_SOURCE.zip`  
Baseline SHA256: `41f4f3990564080325ef634befc45e9192c72248519833b9c3829e1294408e72`

The goal is release polish and a clean build — **NOT** a refactor, redesign, modernization pass, warning cleanup, or architecture change.

## Absolute rules — DO NOT VIOLATE

1. **DO NOT refactor `XESM3ResourceRedirector.cpp`.**
2. **DO NOT change hook addresses, hashes, offsets, calling conventions, detour targets, resolver behavior, archive-scoping behavior, shadow-resource behavior, or resource ownership/lifetime logic.**
3. **DO NOT remove any hook or attach call from `StartXESM3`.**
4. Historical names containing `Diagnostic`, `Probe`, or `Trace` do **NOT** mean the code is safe to delete. In particular, do not remove or redesign calls such as:
   - `AttachApkfHandlerRegistrationTraceDetours()`
   - `AttachGenericApkfDispatchProbeDetour()`
   - `AttachNativeAnimRuntimeDiagnosticDetours()`
   - `AttachRenderMeshProbeDetour()`
   These are part of the proven build unless a compile error directly proves otherwise.
5. **DO NOT change the supported resource routes:** MESH, MAT, TEX, ANIM, SKEL, ASKL.
6. **DO NOT add a generalized numeric mod-priority system.** Public config behavior is strictly:
   - `0 = Disabled`
   - `100 = Enabled`
7. **DO NOT change the Game.exe compatibility gate** or its proven PE32/x86 entry-point check.
8. **DO NOT change the `dbghelp.dll` bootstrap architecture.** `dbghelp.dll` must load `xeSM3.dll` beside `Game.exe` and forward the real system `dbghelp.dll!SymInitialize`.
9. **DO NOT add a dependency on RaimiHook or `d3d9.dll`.** RaimiHook remains optional and separate.
10. **DO NOT add startup popups, success popups, diagnostic MessageBoxes, console windows, or persistent resource logs to Release.**
11. **DO NOT delete the three catalog files in `Mods`:**
    - `filelist.txt`
    - `filelist.apkf.txt`
    - `filelist.apkf.paths.txt`
12. **DO NOT move the official examples into `Mods` by default.** They must remain opt-in examples.
13. **DO NOT update the bundled Detours package or change toolchains unless the current Visual Studio 2022 v143/x86 build cannot compile as provided.**
14. If a requested cleanup would require changing frozen loader logic, **STOP and report it instead of guessing.**

## Public branding

Use the public product name exactly as:

**xeSM3**

Public version:

**v0.1.0**

Author credit:

**Created by TSGAMING264**

Internal legacy identifiers such as `XESM3_*` exported function names, C++ type names, project filenames, and historical comments may remain unchanged when renaming them would add risk. Do not perform mass search-and-replace on internal symbols.

The public payload filename should be:

`xeSM3.dll`

The bootstrap filename remains:

`dbghelp.dll`

## Startup popup requirement

The old v0.1.6.1 startup diagnostics must be absent from the public Release build.

The intended safe implementation is already prepared:

- `Release|x86` must **NOT** define `XESM3_BOOT_DIAGNOSTIC` in either `XESM3.vcxproj` or `XESM3Bootstrap.vcxproj`.
- `Debug|x86` may retain `XESM3_BOOT_DIAGNOSTIC` for troubleshooting.
- Do not rewrite loader startup logic just to remove the popups.

Verify the Release preprocessor definitions before building.

## Build target

Build exactly:

- Configuration: `Release`
- Solution platform: `x86`
- MSVC toolset: `v143`
- Architecture: 32-bit / x86 only

Expected output:

```text
bin\x86\Release\dbghelp.dll
bin\x86\Release\xeSM3.dll
```

Both DLLs are mandatory. A build that produces only one DLL is a failure.

## Build procedure

1. Read this entire instruction file.
2. Inspect the current git/file diff before changing anything.
3. Confirm `Release|x86` has no `XESM3_BOOT_DIAGNOSTIC` macro.
4. Confirm `DbgHelpProxy.cpp` loads `xeSM3.dll` from beside the proxy.
5. Confirm `XESM3.vcxproj` outputs `xeSM3.dll` and depends on the bootstrap project.
6. Run `python VALIDATE_RELEASE_CANDIDATE.py` before the build and require `VALIDATION PASS`.
7. Build the complete solution in `Release|x86`.
8. If the environment does not have Windows + Visual Studio/MSVC v143, **do not substitute MinGW/Clang or change the project to make another compiler work**. Run the static validator, report `BUILD NOT RUN - MSVC unavailable`, and leave the source ready for TSGAMING264 to build in Visual Studio.
9. If the MSVC build succeeds, do not make speculative cleanup changes afterward.
10. Run `python VALIDATE_RELEASE_CANDIDATE.py` again.
11. Run the static checks listed below.
12. Report exactly what files were changed and why.

## Allowed fixes if the build fails

You may fix only release/build plumbing issues such as:

- project output filename mismatch (`xeSM3.dll` vs legacy casing),
- missing project dependency,
- bad Release preprocessor definition,
- stale build-script filename/version text,
- path quoting in `.cmd` files,
- missing project inclusion of an existing source/resource file.

Do not modify resource-loader algorithms to resolve a build-system problem.

## Required static verification after build

Confirm all of the following:

- `dbghelp.dll` exists in `bin\x86\Release`.
- `xeSM3.dll` exists in `bin\x86\Release`.
- Both binaries are x86/PE32.
- Release build does not define `XESM3_BOOT_DIAGNOSTIC`.
- `DbgHelpProxy.cpp` still forwards `SymInitialize` to the real system `dbghelp.dll`.
- Bootstrap still loads the payload beside itself.
- No public resource log is introduced.
- `Mods\mods.config.ini` is present and defaults to no enabled mods.
- Config semantics remain only `0` and `100`.
- The three global catalog files remain present.
- `Examples` contains both official example sets.
- No developer stress-test mod is present under `Mods`.

## DO NOT “clean up” these proven behaviors

Do not change working code just because it appears redundant, old, verbose, diagnostic-looking, or oddly named. This project is the product of reverse engineering and runtime proof. Correctness is more important than elegance.

Do not:

- replace Detours with another hooking library,
- consolidate hooks,
- rewrite the resolver,
- rewrite archive matching,
- change threading/maintenance timing,
- remove strict PACK + APKF scoping,
- change mod indexing rules,
- alter shadow allocation/ownership,
- optimize large resource handling,
- remove legacy helper code based only on static appearance.

## Final report to TSGAMING264

After the build, report:

1. `BUILD PASS` or `BUILD FAILED`.
2. Exact output paths for `dbghelp.dll` and `xeSM3.dll`.
3. Confirmation that Release startup diagnostics are compiled out.
4. A concise list of every source/project/script file you changed.
5. Whether any frozen loader-core code was changed. Expected answer: **NO**, except the already-prepared public default-config text block.
6. Any compiler warnings that appear potentially functional. Do not fix harmless warnings just for cleanliness.

Then STOP. Do not create a GitHub repository or redesign the package unless TSGAMING264 explicitly asks for that in a separate step.
