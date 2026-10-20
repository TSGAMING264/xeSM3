#pragma once

// Minimal runtime settings used by the frozen loader core. Public XESM3 keeps
// all reverse-engineering diagnostics and request logging permanently disabled.
struct DebugMenuToggles
{
    bool bResourceLogging = false;
    bool bNativeAnimRuntimeDiagnostics = false;
    bool bNativeAnimEvaluateTrace = false;
};

extern DebugMenuToggles s_DebugMenuToggles;
