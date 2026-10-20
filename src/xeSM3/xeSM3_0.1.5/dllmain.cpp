// xeSM3 - Spider-Man 3 PC Loose Resource Mod Loader
// Created by TSGAMING264
// Public release v0.1.0

#include "XESM3X86Only.hpp"
#include "XESM3Startup.hpp"
#include "XESM3Detours.hpp"
#include "XESM3ResourceRedirector.hpp"
#include "BloomRestoration.hpp"
#include <cstdint>
#include <cstring>

namespace
{
    enum State : LONG { Dormant, Starting, Active, Failed, Unsupported };
    volatile LONG state = Dormant;
    volatile LONG status = ERROR_IO_PENDING;
    volatile LONG stopping = 0;
    using PresentFn = int (__cdecl*)();
    PresentFn originalPresent = reinterpret_cast<PresentFn>(0x008CD650);

    int __cdecl PresentHook()
    {
        BloomRestoration::OnPresent();
        return originalPresent();
    }
    template<typename T> bool Read(uintptr_t address, T& value)
    {
        SIZE_T size = 0;
        return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
            &value, sizeof(value), &size) && size == sizeof(value);
    }
    template<size_t N> bool Match(uintptr_t address, const BYTE (&expected)[N])
    {
        BYTE actual[N] = {};
        return Read(address, actual) && std::memcmp(actual, expected, N) == 0;
    }
    bool CompatibleImage()
    {
        const auto reject = [](const char* reason) {
            (void)reason;
            return false;
        };
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        IMAGE_DOS_HEADER dos = {};
        IMAGE_NT_HEADERS32 nt = {};
        if (!base || !Read(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
            dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000 ||
            !Read(base + static_cast<uintptr_t>(dos.e_lfanew), nt)) return reject("PE headers unavailable or invalid");
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return reject("host is not an x86 PE32 image");
        if (base != 0x00400000 || nt.OptionalHeader.ImageBase != base) return reject("fixed-layout image base differs from 00400000");
        if (nt.OptionalHeader.AddressOfEntryPoint != 0x005CEBE8) return reject("entry RVA differs from 005CEBE8; native layout needs verification");
        if (nt.OptionalHeader.SizeOfImage <= 0xD50D38) return reject("image does not cover the existing fixed-layout range");
        //independent native timer and resource-dispatch operands identify this layout.
        const BYTE timer[] = {0xff,0x15,0xa8,0x04,0xa4,0x00};
        const BYTE resolver[] = {0xa1,0x38,0x0d,0x15,0x01,0x85,0xc0,0x74,0x02};
        if (!Match(0x008C1A4A, timer)) return reject("timer instructions at 008C1A4A differ or are unreadable");
        if (!Match(0x009CA850, resolver)) return reject("resource instructions at 009CA850 differ or are unreadable");
        return true;
    }
    void Fail(DWORD error)
    {
        InterlockedExchange(&status, static_cast<LONG>(error ? error : ERROR_GEN_FAILURE));
        InterlockedExchange(&state, Failed);
    }
    DWORD WINAPI MaintainResources(void*)
    {
        while (!InterlockedCompareExchange(&stopping, 0, 0))
        {
            Sleep(250);
            if (!InterlockedCompareExchange(&stopping, 0, 0))
            {
                try { InitResourceRedirector();
                }
                catch (...) { Fail(ERROR_UNHANDLED_EXCEPTION); return 0; }
            }
        }
        return 0;
    }
    DWORD WINAPI Start(void*)
    {
        //this worker crosses the loader lock before touching hooks or feature state.
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(&Start), &pinned))
        {
            Fail(GetLastError());
            return 0;
        }
        try
        {
            XESM3DetourTransaction transaction;
            if (transaction.status() != NO_ERROR) { Fail(transaction.status()); return 0; }
            AttachApkfHandlerRegistrationTraceDetours();
            AttachGenericApkfDispatchProbeDetour();
            AttachNativeTexRedirectorDetour();
            AttachNativeMatRedirectorDetour();
            AttachNativeMeshRedirectorDetour();
            AttachNativeAnimRedirectorDetour();
#if defined(XESM3_BOOT_DIAGNOSTIC)
            AttachNativeAnimRuntimeDiagnosticDetours();
            AttachRenderMeshProbeDetour();
#endif
            AttachNativeSkelRedirectorDetour();
            BloomRestoration::Attach();
            XESM3_DetourAttach(&reinterpret_cast<PVOID&>(originalPresent), reinterpret_cast<PVOID>(&PresentHook));
            const LONG error = transaction.commit();
            if (error != NO_ERROR) { BloomRestoration::Finish(error); Fail(error); return 0; }
            const bool bloomReady = BloomRestoration::Finish(error);
            InitResourceRedirector();
            HANDLE maintenance = CreateThread(nullptr, 0, MaintainResources, nullptr, 0, nullptr);
            if (!maintenance) { Fail(GetLastError()); return 0; }
            CloseHandle(maintenance);
            InterlockedExchange(&status, bloomReady ? NO_ERROR : ERROR_NOT_SUPPORTED);
            InterlockedExchange(&state, Active);
        }
        catch (...) { Fail(ERROR_UNHANDLED_EXCEPTION); }
        return 0;
    }
}
void XESM3_RequestStart() noexcept
{
    if (InterlockedCompareExchange(&state, Starting, Dormant) != Dormant) return;
    if (!CompatibleImage())
    {
        InterlockedExchange(&status, ERROR_BAD_EXE_FORMAT);
        InterlockedExchange(&state, Unsupported);
        return;
    }
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&XESM3_RequestStart), &pinned))
    {
        Fail(GetLastError());
        return;
    }
    HANDLE worker = CreateThread(nullptr, 0, Start, nullptr, 0, nullptr);
    if (!worker) Fail(GetLastError());
    else CloseHandle(worker);
}
bool XESM3_IsActive() noexcept
{
    return InterlockedCompareExchange(&state, 0, 0) == Active;
}
DWORD XESM3_StartupStatus() noexcept
{
    return static_cast<DWORD>(InterlockedCompareExchange(&status, 0, 0));
}
void XESM3_StopMaintenance() noexcept
{
    InterlockedExchange(&stopping, 1);
}
extern "C" BOOL WINAPI XESM3_IsLoaded()
{
    return XESM3_IsActive() ? TRUE : FALSE;
}
extern "C" DWORD WINAPI XESM3_GetStartupStatus()
{
    return XESM3_StartupStatus();
}
extern "C" void WINAPI XESM3_ReloadMods()
{
    if (XESM3_IsActive()) ReloadMods();
}
