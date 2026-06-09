#pragma once

// Persistent settings in HKCU\Software\mstsfence (REG_DWORD, 1 = on). The controller
// (mstsfence.exe) writes them from the Settings dialog; the hook DLL reads them to decide
// what to do inside mstsc. Fence/DarkMode default ON when missing; DpiFix defaults OFF.
namespace mstsfence
{
    bool FenceEnabled();     // clamp mstsc to the work area (taskbar fencing)
    bool DarkModeEnabled();  // dark-style mstsc's UI
    bool DpiFixEnabled();    // send the client's DPI to the host at connect (default OFF; hidden)

    void SetFenceEnabled(bool on);
    void SetDarkModeEnabled(bool on);
    void SetDpiFixEnabled(bool on);
}
