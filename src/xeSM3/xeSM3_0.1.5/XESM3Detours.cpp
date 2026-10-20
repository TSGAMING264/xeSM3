#include "XESM3Detours.hpp"
#include <TlHelp32.h>
#include <cstdint>
#include <cstring>

namespace
{
    constexpr size_t MaxHooks = 64;
    constexpr size_t MaxThreads = 1024;
    constexpr size_t EntryBytes = 32;
    SRWLOCK transactionLock = SRWLOCK_INIT;
    volatile LONG owner = 0;
    LONG pendingError = NO_ERROR;
    struct Entry { void* address; BYTE bytes[EntryBytes]; };
    Entry entries[MaxHooks] = {};
    size_t entryCount = 0;

    bool Owned()
    {
        return static_cast<DWORD>(InterlockedCompareExchange(&owner, 0, 0)) == GetCurrentThreadId();
    }
    bool Read(const void* address, void* buffer, SIZE_T size)
    {
        SIZE_T actual = 0;
        return ReadProcessMemory(GetCurrentProcess(), address, buffer, size, &actual) && actual == size;
    }
    bool Executable(const void* address)
    {
        MEMORY_BASIC_INFORMATION info = {};
        if (!VirtualQuery(address, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
        const DWORD p = info.Protect & 0xff;
        return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
            p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
    }
    PVOID ResolveTarget(PVOID target)
    {
        __try { return DetourCodeFromPointer(target, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    }
    LONG Fail(LONG error)
    {
        if (pendingError == NO_ERROR) pendingError = error;
        return error;
    }
    void Release()
    {
        entryCount = 0;
        InterlockedExchange(&owner, 0);
        ReleaseSRWLockExclusive(&transactionLock);
    }
    struct ThreadHandles
    {
        HANDLE values[MaxThreads] = {};
        size_t count = 0;
        ~ThreadHandles()
        {
            for (size_t i = 0; i < count; ++i) CloseHandle(values[i]);
        }
        LONG collect()
        {
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot == INVALID_HANDLE_VALUE) return GetLastError();
            THREADENTRY32 entry = {};
            entry.dwSize = sizeof(entry);
            BOOL found = Thread32First(snapshot, &entry);
            LONG error = found ? NO_ERROR : GetLastError();
            const DWORD process = GetCurrentProcessId();
            const DWORD current = GetCurrentThreadId();
            while (found)
            {
                if (entry.th32OwnerProcessID == process && entry.th32ThreadID != current)
                {
                    if (count == MaxThreads) { error = ERROR_TOO_MANY_THREADS; break; }
                    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                        THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE,
                        FALSE, entry.th32ThreadID);
                    if (!thread)
                    {
                        error = GetLastError();
                        if (error != ERROR_INVALID_PARAMETER) break;
                        error = NO_ERROR;
                    }
                    else values[count++] = thread;
                }
                entry.dwSize = sizeof(entry);
                found = Thread32Next(snapshot, &entry);
                if (!found && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
            }
            CloseHandle(snapshot);
            return error;
        }
        LONG enlist()
        {
            for (size_t i = 0; i < count; ++i)
            {
                if (WaitForSingleObject(values[i], 0) == WAIT_OBJECT_0) continue;
                const LONG error = DetourUpdateThread(values[i]);
                if (error != NO_ERROR) return error;
            }
            return NO_ERROR;
        }
    };
}
LONG WINAPI XESM3_DetourBegin()
{
    if (!TryAcquireSRWLockExclusive(&transactionLock)) return ERROR_BUSY;
    const LONG error = DetourTransactionBegin();
    if (error != NO_ERROR)
    {
        ReleaseSRWLockExclusive(&transactionLock);
        return error;
    }
    entryCount = 0;
    pendingError = NO_ERROR;
    InterlockedExchange(&owner, static_cast<LONG>(GetCurrentThreadId()));
    return NO_ERROR;
}
LONG WINAPI XESM3_DetourAttach(PVOID* original, PVOID hook)
{
    if (!Owned()) return ERROR_INVALID_OPERATION;
    if (pendingError != NO_ERROR) return pendingError;
    PVOID target = nullptr;
    if (!original || !Read(original, &target, sizeof(target)) ||
        !Executable(target) || !Executable(hook)) return Fail(ERROR_INVALID_ADDRESS);
    if (entryCount == MaxHooks) return Fail(ERROR_BUFFER_OVERFLOW);
    PVOID realTarget = ResolveTarget(target);
    if (!Executable(realTarget)) return Fail(ERROR_INVALID_ADDRESS);
    MEMORY_BASIC_INFORMATION region = {};
    if (!VirtualQuery(realTarget, &region, sizeof(region))) return Fail(ERROR_INVALID_ADDRESS);
    if (region.Type == MEM_IMAGE)
    {
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(realTarget), &pinned)) return Fail(GetLastError());
    }
    Entry& entry = entries[entryCount];
    entry.address = realTarget;
    if (!Read(realTarget, entry.bytes, sizeof(entry.bytes))) return Fail(ERROR_INVALID_ADDRESS);
    const LONG error = DetourAttach(original, hook);
    if (error != NO_ERROR) return Fail(error);
    ++entryCount;
    return NO_ERROR;
}
LONG WINAPI XESM3_DetourUpdateThread(HANDLE thread)
{
    if (!Owned()) return ERROR_INVALID_OPERATION;
    //peer threads are collected together immediately before commit.
    return thread == GetCurrentThread() ? NO_ERROR : Fail(ERROR_INVALID_PARAMETER);
}
LONG WINAPI XESM3_DetourAbort()
{
    if (!Owned()) return ERROR_INVALID_OPERATION;
    const LONG error = DetourTransactionAbort();
    Release();
    return error;
}
LONG WINAPI XESM3_DetourCommit()
{
    if (!Owned()) return ERROR_INVALID_OPERATION;
    LONG error = pendingError;
    ThreadHandles threads;
    if (error == NO_ERROR) error = threads.collect();
    if (error == NO_ERROR) error = threads.enlist();
    for (size_t i = 0; error == NO_ERROR && i < entryCount; ++i)
    {
        BYTE current[EntryBytes] = {};
        if (!Read(entries[i].address, current, sizeof(current)) ||
            std::memcmp(current, entries[i].bytes, sizeof(current)) != 0)
            error = ERROR_INVALID_DATA;
    }
    //no game callbacks, file work, or container allocation while peers are stopped.
    if (error == NO_ERROR) error = DetourTransactionCommit();
    else DetourTransactionAbort();
    Release();
    return error;
}
