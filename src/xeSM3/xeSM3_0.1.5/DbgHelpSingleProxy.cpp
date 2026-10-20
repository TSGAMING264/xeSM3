#include "XESM3X86Only.hpp"
#include "XESM3Startup.hpp"
#include <Windows.h>
#include <cstdlib>

extern "C" void __cdecl DbgHelpResolve(unsigned int index);
#define XESM3_DBGHELP(index, ordinal, name) extern "C" void DbgHelpLazy_##index();
#include "DbgHelpExports.inc"
#undef XESM3_DBGHELP

extern "C" {
    FARPROC dbgHelpTargets[] = {
#define XESM3_DBGHELP(index, ordinal, name) reinterpret_cast<FARPROC>(&DbgHelpLazy_##index),
#include "DbgHelpExports.inc"
#undef XESM3_DBGHELP
    };
}
namespace
{
    struct Export { unsigned short ordinal; const char* name; };
    constexpr Export exports[] = {
#define XESM3_DBGHELP(index, ordinal, name) {ordinal, name},
#include "DbgHelpExports.inc"
#undef XESM3_DBGHELP
    };
    HMODULE self = nullptr;
    PVOID volatile systemModule = nullptr;
    thread_local bool resolving = false;

    __declspec(noreturn) void MissingExport()
    {
        RaiseException(0xC0000139u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        std::abort();
    }
    HMODULE SystemDbgHelp()
    {
        auto module = reinterpret_cast<HMODULE>(InterlockedCompareExchangePointer(&systemModule, nullptr, nullptr));
        if (module) return module;
        wchar_t path[MAX_PATH] = {};
        constexpr wchar_t suffix[] = L"\\dbghelp.dll";
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (!length || length > MAX_PATH - sizeof(suffix) / sizeof(suffix[0])) MissingExport();
        for (size_t i = 0; i < sizeof(suffix) / sizeof(suffix[0]); ++i) path[length + i] = suffix[i];
        //no private once lock: forwarding may be entered under another dll's loader lock.
        module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module || module == self) MissingExport();
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned)) MissingExport();
        auto winner = reinterpret_cast<HMODULE>(InterlockedCompareExchangePointer(&systemModule, module, nullptr));
        FreeLibrary(module);
        return winner ? winner : pinned;
    }
}
extern "C" void __cdecl DbgHelpResolve(unsigned int index)
{
    const DWORD saved = GetLastError();
    if (index >= sizeof(exports) / sizeof(exports[0]) || resolving) MissingExport();
    resolving = true;
    const Export& entry = exports[index];
    const FARPROC function = GetProcAddress(SystemDbgHelp(),
        entry.name ? entry.name : MAKEINTRESOURCEA(entry.ordinal));
    if (!function) MissingExport();
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&dbgHelpTargets[index]),
        reinterpret_cast<PVOID>(function));
    resolving = false;
    //the shared startup gate queues native work; no feature hooks run on this caller.
    XESM3_RequestStart();
    SetLastError(saved);
}

#define XESM3_DBGHELP(index, ordinal, name) \
extern "C" __declspec(naked) void DbgHelp_##index() { \
    __asm jmp dword ptr [dbgHelpTargets + index * 4] \
} \
extern "C" __declspec(naked) void DbgHelpLazy_##index() { \
    __asm pushfd \
    __asm pushad \
    __asm mov ebp, esp \
    __asm sub esp, 528 \
    __asm and esp, -16 \
    __asm fxsave [esp] \
    __asm push index \
    __asm call DbgHelpResolve \
    __asm add esp, 4 \
    __asm fxrstor [esp] \
    __asm mov esp, ebp \
    __asm popad \
    __asm popfd \
    __asm jmp dword ptr [dbgHelpTargets + index * 4] \
}
#include "DbgHelpExports.inc"
#undef XESM3_DBGHELP

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) self = module;
    else if (reason == DLL_PROCESS_DETACH) XESM3_StopMaintenance();
    return TRUE;
}
