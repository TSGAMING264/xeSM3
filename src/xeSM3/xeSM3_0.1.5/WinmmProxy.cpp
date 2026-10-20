#include "XESM3X86Only.hpp"
#include "XESM3Startup.hpp"
#include <Windows.h>
#include <mmsystem.h>
#include <cstdlib>

extern "C" void __cdecl WinmmResolve(unsigned int index);
#define XESM3_WINMM(index, ordinal, name) extern "C" void WinmmLazy_##index();
#include "WinmmExports.inc"
#undef XESM3_WINMM

extern "C" {
    FARPROC winmmTargets[] = {
#define XESM3_WINMM(index, ordinal, name) reinterpret_cast<FARPROC>(&WinmmLazy_##index),
#include "WinmmExports.inc"
#undef XESM3_WINMM
    };
}
namespace
{
    struct Export { unsigned short ordinal; const char* name; };
    constexpr Export exports[] = {
#define XESM3_WINMM(index, ordinal, name) {ordinal, name},
#include "WinmmExports.inc"
#undef XESM3_WINMM
    };
    HMODULE self = nullptr;
    const FARPROC lazyTargets[] = {
#define XESM3_WINMM(index, ordinal, name) reinterpret_cast<FARPROC>(&WinmmLazy_##index),
#include "WinmmExports.inc"
#undef XESM3_WINMM
    };
    PVOID volatile systemModule = nullptr;
    thread_local bool resolving = false;

    __declspec(noreturn) void MissingExport()
    {
        RaiseException(0xC0000139u, EXCEPTION_NONCONTINUABLE, 0, nullptr);
        std::abort();
    }
    HMODULE SystemWinmm()
    {
        auto module = reinterpret_cast<HMODULE>(InterlockedCompareExchangePointer(&systemModule, nullptr, nullptr));
        if (module) return module;
        wchar_t path[MAX_PATH] = {};
        const UINT length = GetSystemDirectoryW(path, MAX_PATH);
        if (!length || length >= MAX_PATH - 11) MissingExport();
        constexpr wchar_t suffix[] = L"\\winmm.dll";
        for (size_t i = 0; i < sizeof(suffix) / sizeof(wchar_t); ++i) path[length + i] = suffix[i];
        //no proxy-owned once lock: another dll may forward while holding the loader lock.
        module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module || module == self) MissingExport();
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned)) MissingExport();
        auto winner = reinterpret_cast<HMODULE>(InterlockedCompareExchangePointer(&systemModule, module, nullptr));
        FreeLibrary(module);
        return winner ? winner : pinned;
    }
    template<typename T> T Native(unsigned int index)
    {
        const auto current = reinterpret_cast<FARPROC>(InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&winmmTargets[index]), nullptr, nullptr));
        if (current != lazyTargets[index]) return reinterpret_cast<T>(current);
        WinmmResolve(index);
        return reinterpret_cast<T>(InterlockedCompareExchangePointer(
            reinterpret_cast<PVOID volatile*>(&winmmTargets[index]), nullptr, nullptr));
    }
}
extern "C" void __cdecl WinmmResolve(unsigned int index)
{
    const DWORD saved = GetLastError();
    if (index >= sizeof(exports) / sizeof(exports[0]) || resolving) MissingExport();
    resolving = true;
    const Export& entry = exports[index];
    const FARPROC function = GetProcAddress(SystemWinmm(),
        entry.name ? entry.name : MAKEINTRESOURCEA(entry.ordinal));
    if (!function) MissingExport();
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&winmmTargets[index]),
        reinterpret_cast<PVOID>(function));
    resolving = false;
    SetLastError(saved);
}

#define XESM3_WINMM(index, ordinal, name) \
extern "C" __declspec(naked) void Winmm_##index() { \
    __asm jmp dword ptr [winmmTargets + index * 4] \
} \
extern "C" __declspec(naked) void WinmmLazy_##index() { \
    __asm pushfd \
    __asm pushad \
    __asm mov ebp, esp \
    __asm sub esp, 528 \
    __asm and esp, -16 \
    __asm fxsave [esp] \
    __asm push index \
    __asm call WinmmResolve \
    __asm add esp, 4 \
    __asm fxrstor [esp] \
    __asm mov esp, ebp \
    __asm popad \
    __asm popfd \
    __asm jmp dword ptr [winmmTargets + index * 4] \
}
#include "WinmmExports.inc"
#undef XESM3_WINMM

extern "C" DWORD WINAPI Proxy_timeGetTime()
{
    const DWORD saved = GetLastError();
    const auto native = Native<decltype(&timeGetTime)>(149);
    XESM3_RequestStart();
    SetLastError(saved);
    return native();
}
extern "C" MMRESULT WINAPI Proxy_timeKillEvent(UINT timer)
{
    const DWORD saved = GetLastError();
    const auto native = Native<decltype(&timeKillEvent)>(150);
    XESM3_RequestStart();
    SetLastError(saved);
    return native(timer);
}
extern "C" MMRESULT WINAPI Proxy_timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK callback, DWORD_PTR user, UINT flags)
{
    const DWORD saved = GetLastError();
    const auto native = Native<decltype(&timeSetEvent)>(151);
    XESM3_RequestStart();
    SetLastError(saved);
    return native(delay, resolution, callback, user, flags);
}
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) self = module;
    else if (reason == DLL_PROCESS_DETACH) XESM3_StopMaintenance();
    return TRUE;
}
