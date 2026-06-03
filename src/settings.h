#pragma once

// Persistent settings in HKCU\Software\mstsfence (REG_DWORD, 1 = on). Both default
// ON when the value is missing. The controller (mstsfence.exe) writes them from the
// Settings dialog; the hook DLL reads them to decide what to do inside mstsc.
namespace mstsfence
{
    bool FenceEnabled();     // clamp mstsc to the work area (taskbar fencing)
    bool DarkModeEnabled();  // dark-style mstsc's UI

    void SetFenceEnabled(bool on);
    void SetDarkModeEnabled(bool on);
}
