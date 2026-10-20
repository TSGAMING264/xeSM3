#pragma once
#include <Windows.h>
#include <detours.h>
LONG WINAPI XESM3_DetourBegin();
LONG WINAPI XESM3_DetourAttach(PVOID* original, PVOID hook);
LONG WINAPI XESM3_DetourUpdateThread(HANDLE thread);
LONG WINAPI XESM3_DetourCommit();
LONG WINAPI XESM3_DetourAbort();
class XESM3DetourTransaction
{
public:
    XESM3DetourTransaction() : status_(XESM3_DetourBegin()), open_(status_ == NO_ERROR) {}
    ~XESM3DetourTransaction() { if (open_) XESM3_DetourAbort(); }
    XESM3DetourTransaction(const XESM3DetourTransaction&) = delete;
    XESM3DetourTransaction& operator=(const XESM3DetourTransaction&) = delete;
    LONG status() const { return status_; }
    LONG commit()
    {
        if (!open_) return status_;
        open_ = false;
        return status_ = XESM3_DetourCommit();
    }
private:
    LONG status_;
    bool open_;
};
