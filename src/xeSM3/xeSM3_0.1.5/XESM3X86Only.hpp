#pragma once

// xeSM3 v0.1.0 public loader is intentionally x86/Win32 only.
// Spider-Man 3 PC and the reference exWoS loader are 32-bit processes.
// Refuse to compile if the Visual Studio project is ever switched to x64/ARM.
#if !defined(_M_IX86)
#error xeSM3 supports x86 only. Use Release|x86 or Debug|x86 in the solution (internally mapped to Win32).
#endif

static_assert(sizeof(void*) == 4, "xeSM3 requires a 32-bit process and 4-byte pointers.");
