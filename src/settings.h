#pragma once

// Persistent settings in HKCU\Software\mstsfence (REG_DWORD, 1 = on). The controller
// (mstsfence.exe) writes them from the Settings dialog; the hook DLL reads them to decide
// what to do inside mstsc. Fence/DarkMode default ON when missing.
namespace mstsfence
{
    bool FenceEnabled();        // clamp mstsc to the work area (taskbar fencing)
    bool DarkModeEnabled();     // dark-style mstsc's UI
    bool DpiOverrideEnabled();  // force the host scale to a chosen % instead of matching the client
    unsigned DpiOverridePct();  // the override %, clamped to 100..500 (default 100)

    void SetFenceEnabled(bool on);
    void SetDarkModeEnabled(bool on);
    void SetDpiOverrideEnabled(bool on);
    void SetDpiOverridePct(unsigned pct);
}
