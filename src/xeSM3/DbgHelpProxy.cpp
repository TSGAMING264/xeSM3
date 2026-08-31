// xeSM3 - Spider-Man 3 PC Loose Resource Mod Loader
// Created by TSGAMING264
// Public release v0.1.0

#include <Windows.h>
#include <DbgHelp.h>
#include <cstdint>
#include <cwchar>
#include <cstdio>

// xeSM3 v0.1.0 bootstrap.
// Spider-Man 3 imports dbghelp.dll!SymInitialize. This proxy forwards that
// one import to the real system dbghelp.dll and loads xeSM3.dll beside it.
// d3d9.dll remains untouched for the optional original RaimiHook debug menu.

static HMODULE g_proxyModule = nullptr;
static HMODULE g_realDbgHelp = nullptr;
static HMODULE g_xesm3 = nullptr;
static volatile LONG g_bootstrapStarted = 0;
static volatile LONG g_successPopupShown = 0;
static volatile LONG g_failurePopupShown = 0;
static DWORD g_xesm3LoadError = ERROR_SUCCESS;

static HMODULE LoadRealSystemDbgHelp()
{
    if (g_realDbgHelp)
        return g_realDbgHelp;

    wchar_t systemDir[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 14)
        return nullptr;

    wchar_t fullPath[MAX_PATH] = {};
    wcscpy_s(fullPath, systemDir);
    wcscat_s(fullPath, L"\\dbghelp.dll");

    HMODULE loaded = LoadLibraryW(fullPath);
    if (loaded)
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&g_realDbgHelp), loaded, nullptr);
    return g_realDbgHelp ? g_realDbgHelp : loaded;
}

template <typename T>
static T ResolveDbgHelp(const char* name)
{
    HMODULE real = LoadRealSystemDbgHelp();
    return real ? reinterpret_cast<T>(GetProcAddress(real, name)) : nullptr;
}

static bool IsCompatibleGameProcess()
{
    const auto base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleA(nullptr));
    if (!base)
        return false;
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return false;
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    return nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 &&
           nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC &&
           nt->OptionalHeader.AddressOfEntryPoint == 0x005CEBE8;
}

static HMODULE LoadXESM3BesideProxy()
{
    if (g_xesm3)
        return g_xesm3;
    if (!g_proxyModule || !IsCompatibleGameProcess())
        return nullptr;

    wchar_t proxyPath[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(g_proxyModule, proxyPath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        return nullptr;
    wchar_t* slash = wcsrchr(proxyPath, L'\\');
    if (!slash)
        return nullptr;
    *(slash + 1) = L'\0';

    wchar_t xesm3Path[MAX_PATH] = {};
    wcscpy_s(xesm3Path, proxyPath);
    wcscat_s(xesm3Path, L"xeSM3.dll");

    SetLastError(ERROR_SUCCESS);
    HMODULE loaded = LoadLibraryW(xesm3Path);
    g_xesm3LoadError = loaded ? ERROR_SUCCESS : GetLastError();
    if (loaded)
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&g_xesm3), loaded, nullptr);
    return g_xesm3 ? g_xesm3 : loaded;
}

static void ShowFailureOnce()
{
#if defined(XESM3_BOOT_DIAGNOSTIC)
    if (InterlockedExchange(&g_failurePopupShown, 1) != 0)
        return;
    char text[1024] = {};
    sprintf_s(text,
        "xeSM3 bootstrap FAILED to load xeSM3.dll.\n\n"
        "dbghelp.dll proxy IS active inside Game.exe.\n"
        "LoadLibrary error: %lu (0x%08lX)\n\n"
        "Keep dbghelp.dll and xeSM3.dll beside Game.exe.",
        g_xesm3LoadError, g_xesm3LoadError);
    MessageBoxA(nullptr, text, "xeSM3 v0.1.0 Bootstrap Diagnostic", MB_OK | MB_ICONERROR);
#endif
}

static void ShowSuccessOnce()
{
#if defined(XESM3_BOOT_DIAGNOSTIC)
    if (InterlockedExchange(&g_successPopupShown, 1) != 0)
        return;
    MessageBoxA(nullptr,
        "dbghelp.dll proxy is ACTIVE and xeSM3.dll loaded successfully.\n\n"
        "A second xeSM3 status popup should appear after the resource hooks initialize.",
        "xeSM3 v0.1.0 Bootstrap Diagnostic",
        MB_OK | MB_ICONINFORMATION);
#endif
}

static void EnsureBootstrap()
{
    LoadRealSystemDbgHelp();
    if (LoadXESM3BesideProxy())
        ShowSuccessOnce();
    else if (IsCompatibleGameProcess())
        ShowFailureOnce();
}

static DWORD WINAPI BootstrapThread(LPVOID)
{
    if (InterlockedExchange(&g_bootstrapStarted, 1) != 0)
        return 0;
    EnsureBootstrap();
    return 0;
}

extern "C" BOOL WINAPI Proxy_SymInitialize(HANDLE hProcess, PCSTR UserSearchPath, BOOL fInvadeProcess)
{
    EnsureBootstrap();
    using Fn = BOOL (WINAPI*)(HANDLE, PCSTR, BOOL);
    static Fn fn = nullptr;
    if (!fn)
        fn = ResolveDbgHelp<Fn>("SymInitialize");
    return fn ? fn(hProcess, UserSearchPath, fInvadeProcess) : FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_proxyModule = module;
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, BootstrapThread, nullptr, 0, nullptr);
    }
    return TRUE;
}
