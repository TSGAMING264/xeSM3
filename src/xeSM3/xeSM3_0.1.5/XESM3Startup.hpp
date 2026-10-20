#pragma once
#include <Windows.h>
void XESM3_RequestStart() noexcept;
DWORD XESM3_StartupStatus() noexcept;
bool XESM3_IsActive() noexcept;
void XESM3_StopMaintenance() noexcept;
