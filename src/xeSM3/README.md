# xeSM3 v0.1.5 source

This directory contains the supplied v0.1.5 single-DLL Windows x86 source. `xeSM3_0.1.5/` is the Visual Studio project, and `BUILD_SINGLE_DBGHELP.cmd` builds the Release/Win32 `dbghelp` proxy. Its outputs are `build/output/dbghelp.dll` and `build/output/xeSM3.ini` by default; the script accepts an alternate output directory as its first argument.

Visual Studio 2022 C++ x86 build tools and a Windows SDK are required. The source includes the Detours dependency and its license. Generated objects, PDBs, DLLs, user settings, and local build output are not tracked. The historical opt-in resource examples remain in `Examples/`.

The published release DLL comes from the supplied authoritative build, not from a newly verified rebuild of this source. Do not substitute a fresh output for that DLL without release validation.
