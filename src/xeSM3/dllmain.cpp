// xeSM3 - Spider-Man 3 PC Loose Resource Mod Loader
// Created by TSGAMING264
// Public release v0.1.0

#include "XESM3X86Only.hpp"
#include <Windows.h>
#include <cstdint>
#include <cstdio>
#include <detours.h>

#include "XESM3ResourceRedirector.hpp"

static volatile LONG g_Started = 0;
static volatile LONG g_StatusPopupShown = 0;

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
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_I386)
        return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return false;

    // Proven retail Spider-Man 3 PC executable target.
    return nt->OptionalHeader.AddressOfEntryPoint == 0x005CEBE8;
}

static DWORD WINAPI ResourceResolverMaintenance(LPVOID)
{
    for (;;)
    {
        Sleep(250);
        InitResourceRedirector();
    }
}

static DWORD WINAPI StartXESM3(LPVOID)
{
    if (InterlockedExchange(&g_Started, 1) != 0)
        return 0;

    if (!IsCompatibleGameProcess())
    {
#if defined(XESM3_BOOT_DIAGNOSTIC)
        MessageBoxA(nullptr,
            "xeSM3.dll loaded, but Game.exe compatibility validation FAILED.",
            "xeSM3 v0.1.0 Loader Diagnostic",
            MB_OK | MB_ICONERROR);
#endif
        return 0;
    }

    const LONG beginResult = DetourTransactionBegin();
    if (beginResult != NO_ERROR)
    {
#if defined(XESM3_BOOT_DIAGNOSTIC)
        char text[256] = {};
        sprintf_s(text, "DetourTransactionBegin failed: %ld", beginResult);
        MessageBoxA(nullptr, text, "xeSM3 v0.1.0 Loader Diagnostic", MB_OK | MB_ICONERROR);
#endif
        return static_cast<DWORD>(beginResult);
    }

    DetourUpdateThread(GetCurrentThread());

    // Frozen RH717.4.0 resource routes only. No debug-menu/gameplay hooks.
    AttachApkfHandlerRegistrationTraceDetours();
    AttachGenericApkfDispatchProbeDetour();
    AttachNativeTexRedirectorDetour();
    AttachNativeMatRedirectorDetour();
    AttachNativeMeshRedirectorDetour();
    AttachNativeAnimRedirectorDetour();
    AttachNativeAnimRuntimeDiagnosticDetours();
    AttachRenderMeshProbeDetour();
    AttachNativeSkelRedirectorDetour();

    const LONG result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
#if defined(XESM3_BOOT_DIAGNOSTIC)
        char text[256] = {};
        sprintf_s(text, "DetourTransactionCommit failed: %ld (0x%08lX)", result, result);
        MessageBoxA(nullptr, text, "xeSM3 v0.1.0 Loader Diagnostic", MB_OK | MB_ICONERROR);
#endif
        return static_cast<DWORD>(result);
    }

    InitResourceRedirector();

#if defined(XESM3_BOOT_DIAGNOSTIC)
    // Give ReloadMods/strict index construction a moment to finish, then show
    // one in-memory status report. No resource-log file is created.
    Sleep(100);
    InitResourceRedirector();

    if (InterlockedExchange(&g_StatusPopupShown, 1) == 0)
    {
        char text[1600] = {};
        sprintf_s(text,
            "xeSM3 v0.1.0 is ACTIVE inside Game.exe.\n\n"
            "Detours commit: PASS (%ld)\n"
            "Enabled mod packages: %ld\n"
            "Indexed loose resources: %ld\n\n"
            "Hooks:\n"
            "  Resource resolver: %s\n"
            "  MESH: %s\n"
            "  MAT: %s\n"
            "  ANIM: %s\n"
            "  TEX: %s\n"
            "  SKEL: %s\n\n"
            "Expected bundled test: 1 enabled mod / 2 loose resources.",
            result,
            XESM3_GetEnabledModPackageCountForDiagnostics(),
            XESM3_GetIndexedResourceCountForDiagnostics(),
            IsResourceRedirectorInstalled() ? "ON" : "WAITING",
            IsNativeMeshRedirectorInstalled() ? "ON" : "OFF",
            IsNativeMatRedirectorInstalled() ? "ON" : "OFF",
            IsNativeAnimRedirectorInstalled() ? "ON" : "OFF",
            IsNativeTexRedirectorInstalled() ? "ON" : "OFF",
            IsNativeSkelRedirectorInstalled() ? "ON" : "OFF");
        MessageBoxA(nullptr, text, "xeSM3 v0.1.0 Loader Diagnostic", MB_OK | MB_ICONINFORMATION);
    }
#endif

    CreateThread(nullptr, 0, ResourceResolverMaintenance, nullptr, 0, nullptr);
    return 0;
}

extern "C" BOOL WINAPI XESM3_IsLoaded()
{
    return TRUE;
}

extern "C" void WINAPI XESM3_ReloadMods()
{
    ReloadMods();
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        if (IsCompatibleGameProcess())
            CreateThread(nullptr, 0, StartXESM3, nullptr, 0, nullptr);
    }
    return TRUE;
}
