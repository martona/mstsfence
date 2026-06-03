// wmltahook.dll -- injected into mstsc by the controller's global WH_CBT hook
// (inert in every other process; DllMain filters by exe name). It makes mstsc size
// its full-screen window and negotiate its session resolution to the monitor work
// area instead of the whole monitor, so the host taskbar stays visible at 1:1.
//
// mstsc reads the screen size from more than one API, and WHICH one drives the
// sizing varies by environment (monitor count, nested/virtual displays). So we
// clamp every reader mstsc is seen to use: GetMonitorInfoW/A (rcMonitor := rcWork),
// EnumDisplayMonitors (callback rect := work area), EnumDisplaySettingsW (dmPels :=
// per-device work area), and GetSystemMetrics (SM_C*SCREEN := primary work area;
// SM_C*VIRTUALSCREEN only when single-monitor, else multimon placement breaks).
//
// Light log-once tracing goes to %TEMP%\wmltahook.log when WMLTA_TRACE=1; silent otherwise.

#include <windows.h>

// vcpkg's detours port may expose the header either as <detours.h> or namespaced
// under <detours/detours.h> depending on how it's consumed; accept both.
#if __has_include(<detours.h>)
#  include <detours.h>
#elif __has_include(<detours/detours.h>)
#  include <detours/detours.h>
#else
#  error "detours.h not found -- check the vcpkg detours dependency"
#endif

#include <cstdarg>
#include <cstdio>
#include <cwchar>

#include "darkmode.h"

static bool g_isMstsc = false;
static LONG g_installed = 0;
static bool g_trace = false;      // %TEMP%\wmltahook.log + OutputDebugString; off unless WMLTA_TRACE is set
static bool g_swpHooked = false;  // SetWindowPos observe-hook is only installed when tracing

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------
static void LogV(const wchar_t* fmt, va_list ap)
{
    wchar_t buf[1024];
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    size_t len = wcsnlen(buf, ARRAYSIZE(buf));
    if (len == 0)
    {
        return;
    }
    OutputDebugStringW(buf);

    wchar_t path[MAX_PATH];
    DWORD t = GetTempPathW(ARRAYSIZE(path), path);
    if (t == 0 || t > ARRAYSIZE(path) || wcscat_s(path, L"wmltahook.log") != 0)
    {
        return;
    }
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        return;
    }
    char u8[2300];
    int b = WideCharToMultiByte(CP_UTF8, 0, buf, static_cast<int>(len), u8,
                                static_cast<int>(sizeof(u8)) - 2, nullptr, nullptr);
    if (b > 0)
    {
        u8[b] = '\r';
        u8[b + 1] = '\n';
        DWORD written = 0;
        WriteFile(h, u8, static_cast<DWORD>(b) + 2, &written, nullptr);
    }
    CloseHandle(h);
}

static void Log(const wchar_t* fmt, ...)
{
    if (!g_trace)
    {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

// Log only the first time a given call site is hit (keeps the log readable).
#define LOG_ONCE(...)                                          \
    do                                                         \
    {                                                          \
        static LONG _once = 0;                                 \
        if (InterlockedExchange(&_once, 1) == 0) Log(__VA_ARGS__); \
    } while (0)

// ---------------------------------------------------------------------------
// real function pointers
// ---------------------------------------------------------------------------
static BOOL(WINAPI* Real_GetMonitorInfoW)(HMONITOR, LPMONITORINFO) = GetMonitorInfoW;
static BOOL(WINAPI* Real_GetMonitorInfoA)(HMONITOR, LPMONITORINFO) = GetMonitorInfoA;
static BOOL(WINAPI* Real_EnumDisplayMonitors)(HDC, LPCRECT, MONITORENUMPROC, LPARAM) = EnumDisplayMonitors;
static int(WINAPI* Real_GetSystemMetrics)(int) = GetSystemMetrics;
static BOOL(WINAPI* Real_EnumDisplaySettingsW)(LPCWSTR, DWORD, DEVMODEW*) = EnumDisplaySettingsW;
static BOOL(WINAPI* Real_SetWindowPos)(HWND, HWND, int, int, int, int, UINT) = SetWindowPos;

// ---------------------------------------------------------------------------
// helpers: work-area dimensions (via the REAL GetMonitorInfo -> no recursion,
// and rcWork is the field we never rewrite)
// ---------------------------------------------------------------------------
static bool WorkAreaOf(HMONITOR hMon, LONG* w, LONG* h)
{
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!hMon || !Real_GetMonitorInfoW(hMon, &mi))
    {
        return false;
    }
    *w = mi.rcWork.right - mi.rcWork.left;
    *h = mi.rcWork.bottom - mi.rcWork.top;
    return true;
}

static bool PrimaryWorkArea(LONG* w, LONG* h)
{
    POINT origin{ 0, 0 };  // the primary monitor's top-left is (0,0) by definition
    return WorkAreaOf(MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY), w, h);
}

// Work area of the monitor whose device name matches `device` (e.g. "\\.\DISPLAY2"),
// or the primary when device is null/empty. Lets EnumDisplaySettings clamp to the
// right monitor under multi-monitor instead of always the primary.
struct DevMatch
{
    LPCWSTR device;
    LONG w;
    LONG h;
    bool found;
};

static BOOL CALLBACK DevMatchProc(HMONITOR hMon, HDC, LPRECT, LPARAM lp)
{
    DevMatch* d = reinterpret_cast<DevMatch*>(lp);
    MONITORINFOEXW mi;  // MONITORINFOEXW publicly derives MONITORINFO in C++
    mi.cbSize = sizeof(mi);
    if (Real_GetMonitorInfoW(hMon, &mi) && _wcsicmp(mi.szDevice, d->device) == 0)
    {
        d->w = mi.rcWork.right - mi.rcWork.left;
        d->h = mi.rcWork.bottom - mi.rcWork.top;
        d->found = true;
        return FALSE;  // found it; stop enumerating
    }
    return TRUE;
}

static bool WorkAreaForDevice(LPCWSTR device, LONG* w, LONG* h)
{
    if (!device || !device[0])
    {
        return PrimaryWorkArea(w, h);
    }
    DevMatch d{ device, 0, 0, false };
    Real_EnumDisplayMonitors(nullptr, nullptr, DevMatchProc, reinterpret_cast<LPARAM>(&d));
    if (d.found)
    {
        *w = d.w;
        *h = d.h;
        return true;
    }
    return PrimaryWorkArea(w, h);
}

// ---------------------------------------------------------------------------
// hooks -- each makes its API report the work area instead of the full monitor
// ---------------------------------------------------------------------------
static BOOL WINAPI Hooked_GetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFO lpmi)
{
    BOOL ok = Real_GetMonitorInfoW(hMonitor, lpmi);
    if (ok && lpmi)
    {
        LOG_ONCE(L"GetMonitorInfoW rcMon=(%ld,%ld,%ld,%ld) rcWork=(%ld,%ld,%ld,%ld) -> rcMon:=rcWork",
                 lpmi->rcMonitor.left, lpmi->rcMonitor.top, lpmi->rcMonitor.right, lpmi->rcMonitor.bottom,
                 lpmi->rcWork.left, lpmi->rcWork.top, lpmi->rcWork.right, lpmi->rcWork.bottom);
        lpmi->rcMonitor = lpmi->rcWork;
    }
    return ok;
}

static BOOL WINAPI Hooked_GetMonitorInfoA(HMONITOR hMonitor, LPMONITORINFO lpmi)
{
    BOOL ok = Real_GetMonitorInfoA(hMonitor, lpmi);
    if (ok && lpmi)
    {
        lpmi->rcMonitor = lpmi->rcWork;
    }
    return ok;
}

// The EnumDisplayMonitors callback receives the *monitor* rect directly (never via
// GetMonitorInfo), and some mstsc paths (notably on single / nested displays) size
// from this rather than GetMonitorInfo -- so hand the caller the work area instead.
struct EdmCtx
{
    MONITORENUMPROC proc;
    LPARAM lparam;
};

static BOOL CALLBACK EdmThunk(HMONITOR hMonitor, HDC hdc, LPRECT rc, LPARAM lp)
{
    EdmCtx* ctx = reinterpret_cast<EdmCtx*>(lp);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (rc && Real_GetMonitorInfoW(hMonitor, &mi))
    {
        LOG_ONCE(L"EnumDisplayMonitors rc=(%ld,%ld,%ld,%ld) -> work=(%ld,%ld,%ld,%ld)",
                 rc->left, rc->top, rc->right, rc->bottom, mi.rcWork.left, mi.rcWork.top,
                 mi.rcWork.right, mi.rcWork.bottom);
        *rc = mi.rcWork;
    }
    return ctx->proc(hMonitor, hdc, rc, ctx->lparam);
}

static BOOL WINAPI Hooked_EnumDisplayMonitors(HDC hdc, LPCRECT clip, MONITORENUMPROC proc, LPARAM lparam)
{
    EdmCtx ctx{ proc, lparam };
    return Real_EnumDisplayMonitors(hdc, clip, EdmThunk, reinterpret_cast<LPARAM>(&ctx));
}

static int WINAPI Hooked_GetSystemMetrics(int index)
{
    int v = Real_GetSystemMetrics(index);
    LONG w, h;
    switch (index)
    {
    case SM_CXSCREEN:  // SM_C*SCREEN are the primary monitor, by definition
        if (PrimaryWorkArea(&w, &h))
        {
            LOG_ONCE(L"GetSystemMetrics(CXSCREEN) %d -> %ld (work)", v, w);
            return static_cast<int>(w);
        }
        break;
    case SM_CYSCREEN:
        if (PrimaryWorkArea(&w, &h))
        {
            LOG_ONCE(L"GetSystemMetrics(CYSCREEN) %d -> %ld (work)", v, h);
            return static_cast<int>(h);
        }
        break;
    case SM_CXVIRTUALSCREEN:
    case SM_CYVIRTUALSCREEN:
        // The virtual screen spans ALL monitors; clamping it on multi-monitor
        // breaks placement (origin + clamped-size lands on the wrong monitor).
        // Only safe when there is a single monitor (virtual == that monitor).
        if (Real_GetSystemMetrics(SM_CMONITORS) == 1 && PrimaryWorkArea(&w, &h))
        {
            LOG_ONCE(L"GetSystemMetrics(VIRTUAL %d) %d -> %ld (work)", index, v,
                     index == SM_CXVIRTUALSCREEN ? w : h);
            return static_cast<int>(index == SM_CXVIRTUALSCREEN ? w : h);
        }
        break;
    default:
        break;
    }
    return v;
}

static BOOL WINAPI Hooked_EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* dm)
{
    BOOL ok = Real_EnumDisplaySettingsW(device, mode, dm);
    if (ok && dm && mode == ENUM_CURRENT_SETTINGS)
    {
        LONG w, h;
        if (WorkAreaForDevice(device, &w, &h))
        {
            LOG_ONCE(L"EnumDisplaySettingsW(%s) %lux%lu -> %ldx%ld (work)",
                     device ? device : L"(null)", dm->dmPelsWidth, dm->dmPelsHeight, w, h);
            dm->dmPelsWidth = static_cast<DWORD>(w);
            dm->dmPelsHeight = static_cast<DWORD>(h);
        }
    }
    return ok;
}

// Observe only (no clamp): confirms the size mstsc ultimately picks for its
// full-screen window after the readers above are clamped.
static BOOL WINAPI Hooked_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if ((flags & SWP_NOSIZE) == 0 && cx >= 2000 && cy >= 1500)
    {
        // log EVERY big resize (not once) so the final full-screen call is visible
        Log(L"SetWindowPos hwnd=%p pos=(%d,%d) size=%dx%d flags=0x%X", hwnd, x, y, cx, cy, flags);
    }
    return Real_SetWindowPos(hwnd, after, x, y, cx, cy, flags);
}

// ---------------------------------------------------------------------------
// install / uninstall
// ---------------------------------------------------------------------------
static void EnsureHooksInstalled()
{
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0)
    {
        return;
    }
    Log(L"installing hooks...");
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetMonitorInfoW), Hooked_GetMonitorInfoW);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetMonitorInfoA), Hooked_GetMonitorInfoA);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_EnumDisplayMonitors), Hooked_EnumDisplayMonitors);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetSystemMetrics), Hooked_GetSystemMetrics);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_EnumDisplaySettingsW), Hooked_EnumDisplaySettingsW);
    if (g_trace)  // observe-only; not needed for the fix, so only attach when tracing
    {
        DetourAttach(&reinterpret_cast<PVOID&>(Real_SetWindowPos), Hooked_SetWindowPos);
        g_swpHooked = true;
    }
    LONG err = DetourTransactionCommit();
    Log(L"DetourTransactionCommit -> %ld", err);
}

static void RemoveHooks()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetMonitorInfoW), Hooked_GetMonitorInfoW);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetMonitorInfoA), Hooked_GetMonitorInfoA);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_EnumDisplayMonitors), Hooked_EnumDisplayMonitors);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetSystemMetrics), Hooked_GetSystemMetrics);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_EnumDisplaySettingsW), Hooked_EnumDisplaySettingsW);
    if (g_swpHooked)
    {
        DetourDetach(&reinterpret_cast<PVOID&>(Real_SetWindowPos), Hooked_SetWindowPos);
    }
    DetourTransactionCommit();
}

extern "C" __declspec(dllexport) LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && g_isMstsc)
    {
        EnsureHooksInstalled();
        wmlta::DarkModeOnCbt(nCode, reinterpret_cast<HWND>(wParam));
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static bool HostIsMstsc()
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return false;
    }
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    return _wcsicmp(base, L"mstsc.exe") == 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID lpReserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinst);
        g_isMstsc = HostIsMstsc();
        if (g_isMstsc)
        {
            g_trace = GetEnvironmentVariableW(L"WMLTA_TRACE", nullptr, 0) > 0;
            Log(L"ATTACH pid=%lu (mstsc) -- DLL is in. (trace=%d)", GetCurrentProcessId(), g_trace);
            wmlta::DarkModeOnAttach();
        }
        break;

    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr && g_isMstsc && g_installed)
        {
            RemoveHooks();
        }
        break;
    }
    return TRUE;
}
