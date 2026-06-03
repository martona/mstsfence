#pragma once

#include <windows.h>

// Dark-styling of mstsc's UI via umbra, driven by the controller's global WH_CBT
// hook. These are only ever called from inside mstsc (hook.cpp gates by exe name);
// theming can be disabled at runtime with MSTSFENCE_NODARK=1 in mstsc's environment.
namespace mstsfence
{
    void DarkModeOnAttach();                  // call once from DllMain(PROCESS_ATTACH)
    void DarkModeOnCbt(int code, HWND hwnd);  // call from the WH_CBT proc (HCBT_* codes)
}
