// mstsfencehook.dll -- injected into mstsc by the controller's global WH_CBT hook
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
// Light log-once tracing goes to %TEMP%\mstsfencehook.log when MSTSFENCE_TRACE=1; silent otherwise.
//
// (Diagnostic, always-on) A separate set of DPI probes hooks GetDpiForSystem,
// GetDpiForWindow, GetDeviceCaps(LOGPIXELSX), GetDpiForMonitor and GetScaleFactorForMonitor
// and logs EVERY call -- the DPI value and which API returned it (the "source") -- to the
// same sinks, ignoring MSTSFENCE_TRACE, to chase down a DPI-reporting discrepancy inside mstsc.

#include <windows.h>
#include <shellscalingapi.h>   // GetDpiForMonitor / MONITOR_DPI_TYPE (shcore.dll)

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
#include <cstring>
#include <cwchar>

#include "darkmode.h"
#include "settings.h"

static bool g_isMstsc = false;
static LONG g_installed = 0;
static bool g_trace = false;      // %TEMP%\mstsfencehook.log + OutputDebugString; off unless MSTSFENCE_TRACE is set
static bool g_swpHooked = false;  // SetWindowPos observe-hook is only installed when tracing
static bool g_fenceEnabled = false;  // HKCU\Software\mstsfence\Fence (default on); read once at attach
static bool g_logApiProbes = false;  // per-API DPI probes are noise now -> silenced; only the layout hook logs
static bool g_patchLayout = false;   // when true, the layout-send hook may rewrite fields before they go out
static bool g_forceGate = false;     // gate isn't consulted at connect -> flipping it can't help; left off
static bool g_forceSend = true;      // FIX: replay the dance's SendMonitorLayoutPdu(channel,0,0) once at connect

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
    if (t == 0 || t > ARRAYSIZE(path) || wcscat_s(path, L"mstsfencehook.log") != 0)
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

// DPI probes are a deliberately ALWAYS-ON diagnostic: unlike Log() they ignore
// MSTSFENCE_TRACE and fire on every call. We're chasing a specific "what DPI does
// mstsc see, and from which API" question, so every call is recorded (DebugView +
// %TEMP%\mstsfencehook.log).
static void DpiLog(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

// The per-API DPI probes route through here so they can be silenced as a group
// (g_logApiProbes) now that they've served their purpose. The layout-send hook --
// the thing we actually care about -- uses DpiLog directly and always logs.
static void ApiProbeLog(const wchar_t* fmt, ...)
{
    if (!g_logApiProbes)
        return;
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

// DPI probes (observe-only): the APIs mstsc can read its scaling from.
static UINT(WINAPI* Real_GetDpiForSystem)(void) = GetDpiForSystem;
static UINT(WINAPI* Real_GetDpiForWindow)(HWND) = GetDpiForWindow;
static int(WINAPI* Real_GetDeviceCaps)(HDC, int) = GetDeviceCaps;
static HRESULT(WINAPI* Real_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*) = GetDpiForMonitor;
// GetScaleFactorForMonitor returns the shcore DEVICE_SCALE_FACTOR (e.g. 200 for a 200%
// monitor). Despite the similar name this is NOT the RDPEDISP DeviceScaleFactor verbatim:
// mstsc maps it into the PDU as DesktopScaleFactor (100..500, where 200 is perfectly legal)
// PLUS a separate, coarse DeviceScaleFactor that must be 100/140/180 (200% -> 180). We log
// the raw read to see what mstsc starts from when it builds the layout.
static HRESULT(WINAPI* Real_GetScaleFactorForMonitor)(HMONITOR, DEVICE_SCALE_FACTOR*) = GetScaleFactorForMonitor;

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
// DPI probes -- observe-only. Each calls the real API, logs the value it returned
// and which API produced it (the "source"), and returns it UNCHANGED. Logged on
// EVERY call (no LOG_ONCE) so a value that varies between calls is visible. The
// "(N%)" suffix is the scale relative to 96 DPI (96->100%, 120->125%, 144->150%).
// ---------------------------------------------------------------------------
static const wchar_t* MdtName(MONITOR_DPI_TYPE t)
{
    switch (t)
    {
    case MDT_EFFECTIVE_DPI: return L"MDT_EFFECTIVE_DPI";
    case MDT_ANGULAR_DPI:   return L"MDT_ANGULAR_DPI";
    case MDT_RAW_DPI:       return L"MDT_RAW_DPI";
    default:                return L"MDT_?";
    }
}

static UINT WINAPI Hooked_GetDpiForSystem(void)
{
    UINT dpi = Real_GetDpiForSystem();
    ApiProbeLog(L"DPI probe: GetDpiForSystem() -> %u (%d%%)", dpi, MulDiv(static_cast<int>(dpi), 100, 96));
    return dpi;
}

static UINT WINAPI Hooked_GetDpiForWindow(HWND hwnd)
{
    UINT dpi = Real_GetDpiForWindow(hwnd);
    ApiProbeLog(L"DPI probe: GetDpiForWindow(hwnd=%p) -> %u (%d%%)", hwnd, dpi,
           MulDiv(static_cast<int>(dpi), 100, 96));
    return dpi;
}

static int WINAPI Hooked_GetDeviceCaps(HDC hdc, int index)
{
    int v = Real_GetDeviceCaps(hdc, index);
    if (index == LOGPIXELSX)  // only the horizontal-DPI query the caller asked about
    {
        ApiProbeLog(L"DPI probe: GetDeviceCaps(hdc=%p, LOGPIXELSX) -> %d (%d%%)", hdc, v,
               MulDiv(v, 100, 96));
    }
    return v;
}

static HRESULT WINAPI Hooked_GetDpiForMonitor(HMONITOR hmon, MONITOR_DPI_TYPE type, UINT* dpiX, UINT* dpiY)
{
    HRESULT hr = Real_GetDpiForMonitor(hmon, type, dpiX, dpiY);
    UINT x = (SUCCEEDED(hr) && dpiX) ? *dpiX : 0u;
    UINT y = (SUCCEEDED(hr) && dpiY) ? *dpiY : 0u;
    ApiProbeLog(L"DPI probe: GetDpiForMonitor(hMon=%p, %s) -> hr=0x%08lX dpiX=%u dpiY=%u (%d%%)",
           hmon, MdtName(type), static_cast<unsigned long>(hr), x, y,
           x ? MulDiv(static_cast<int>(x), 100, 96) : 0);
    return hr;
}

static HRESULT WINAPI Hooked_GetScaleFactorForMonitor(HMONITOR hmon, DEVICE_SCALE_FACTOR* scale)
{
    HRESULT hr = Real_GetScaleFactorForMonitor(hmon, scale);
    int s = (SUCCEEDED(hr) && scale) ? static_cast<int>(*scale) : 0;
    // s is the shcore DEVICE_SCALE_FACTOR (200 for a 200% monitor) -- a normal, legal value,
    // NOT what goes on the wire verbatim. mstsc maps it to PDU DesktopScaleFactor=<percent>
    // (legal 100..500) + a SEPARATE DeviceScaleFactor companion that must be 100/140/180.
    ApiProbeLog(L"DPI probe: GetScaleFactorForMonitor(hMon=%p) -> hr=0x%08lX scale=%d%% "
           L"(legal desktop scale; the PDU's separate DeviceScaleFactor companion must be 100/140/180)",
           hmon, static_cast<unsigned long>(hr), s);
    return hr;
}

// ---------------------------------------------------------------------------
// install / uninstall
// ---------------------------------------------------------------------------
static void EnsureHooksInstalled()
{
    if (!g_fenceEnabled)
        return;  // taskbar fencing disabled in settings -> leave mstsc's sizing alone
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

// DPI probes install/uninstall -- separate from the fencing hooks above and
// deliberately NOT gated on g_fenceEnabled or g_trace: the diagnostic runs whenever
// the DLL is live inside mstsc, regardless of the fence/trace settings.
static LONG g_dpiInstalled = 0;

static void EnsureDpiProbesInstalled()
{
    if (InterlockedCompareExchange(&g_dpiInstalled, 1, 0) != 0)
    {
        return;
    }
    DpiLog(L"DPI probe: installing (pid=%lu)...", GetCurrentProcessId());
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetDpiForSystem), Hooked_GetDpiForSystem);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetDpiForWindow), Hooked_GetDpiForWindow);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetDeviceCaps), Hooked_GetDeviceCaps);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetDpiForMonitor), Hooked_GetDpiForMonitor);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetScaleFactorForMonitor), Hooked_GetScaleFactorForMonitor);
    LONG err = DetourTransactionCommit();
    DpiLog(L"DPI probe: install commit -> %ld", err);
}

static void RemoveDpiProbes()
{
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetDpiForSystem), Hooked_GetDpiForSystem);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetDpiForWindow), Hooked_GetDpiForWindow);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetDeviceCaps), Hooked_GetDeviceCaps);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetDpiForMonitor), Hooked_GetDpiForMonitor);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetScaleFactorForMonitor), Hooked_GetScaleFactorForMonitor);
    DetourTransactionCommit();
}

// ---------------------------------------------------------------------------
// RDPEDISP monitor-layout send hook -- the actual question. mstscax builds a
// DISPLAYCONTROL_MONITOR_LAYOUT array (MS-RDPEDISP 2.2.2.2) and hands it to
// RdpDisplayControlChannel::SendMonitorLayoutPdu(this, count, monitors) right before
// it goes on the wire. We log the fields that matter (DesktopScaleFactor /
// DeviceScaleFactor / Width / Height) on EVERY send, so the initial-connect PDU and
// the post-"dance" PDU can be diffed -- and can rewrite them in place (g_patchLayout).
// Resolved by RVA, gated on the exact mstscax build (PDB GUID) so an unknown build is
// a safe no-op rather than a wild jump into the wrong bytes.
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct DCML  // DISPLAYCONTROL_MONITOR_LAYOUT, 40 bytes; offsets confirmed from the validator disasm
{
    DWORD Flags;               // +0x00  bit0 = DISPLAYCONTROL_MONITOR_PRIMARY
    LONG  Left;                // +0x04
    LONG  Top;                 // +0x08
    DWORD Width;               // +0x0c  (validator: 200..8192, must be even)
    DWORD Height;              // +0x10  (validator: 200..8192)
    DWORD PhysicalWidth;       // +0x14  mm
    DWORD PhysicalHeight;      // +0x18  mm
    DWORD Orientation;         // +0x1c  0/90/180/270
    DWORD DesktopScaleFactor;  // +0x20  100..500 (your 200)
    DWORD DeviceScaleFactor;   // +0x24  must be 100/140/180 (200% -> 180)
};
#pragma pack(pop)
static_assert(sizeof(DCML) == 40, "DISPLAYCONTROL_MONITOR_LAYOUT must be 40 bytes");

using SendLayoutArr_t  = int (*)(void* thisptr, unsigned int count, void* monitors);   // Send(array) & Write
using SendLayoutWH_t   = int (*)(void* thisptr, unsigned int a, unsigned int b);        // Send(u32,u32)
using ValidateLayout_t = int (*)(unsigned int count, void* monitors, void* arg3);       // Validate
using GfxSetLayout_t   = int (*)(void* thisptr, unsigned int a, void* ptr, void* arg4); // GFX SetMonitorLayout
using GetMatchesLocal_t = HRESULT (*)(void* thisptr, short* pVal);                       // get_RemoteMonitorLayoutMatchesLocal
using ChannelFn3_t      = int (*)(void* thisptr, void* a2, void* a3);                    // OnDataReceived / InitializeSelf

static SendLayoutArr_t  Real_SendLayoutA  = nullptr;  // SendMonitorLayoutPdu(this,count,monitors)
static SendLayoutWH_t   Real_SendLayoutB  = nullptr;  // SendMonitorLayoutPdu(this,u32,u32)
static SendLayoutArr_t  Real_WriteLayout  = nullptr;  // WriteMonitorLayoutPdu(this,count,monitors)
static ValidateLayout_t Real_Validate     = nullptr;  // ValidateDisplayControlMonitorLayout(count,monitors,?)
static GfxSetLayout_t   Real_GfxSetLayout = nullptr;  // RdpGfxClientChannel::SetMonitorLayout(this,u32,ptr,?)
static GetMatchesLocal_t Real_MatchesLocal = nullptr; // the gate: VARIANT_TRUE => mstsc skips the layout send
static ChannelFn3_t Real_OnDataReceived = nullptr;    // RdpDisplayControlChannel::OnDataReceived(this,...)
static ChannelFn3_t Real_InitSelf       = nullptr;    // RdpDisplayControlChannel::InitializeSelf(this,...)
static void* volatile g_displayChannel  = nullptr;    // captured channel* -- a valid 'this' for SendMonitorLayoutPdu
static LONG g_forcedSend = 0;                          // force-send latch (once per process)
static void TryForceSend(void* channel, const wchar_t* from);  // fwd decl (called from the GFX hook below)

// Shared: dump a DISPLAYCONTROL_MONITOR_LAYOUT array, tagged with which hook fired.
static void LogLayoutArray(const wchar_t* tag, unsigned int count, void* monitors)
{
    DCML* m = reinterpret_cast<DCML*>(monitors);
    if (!m || count == 0 || count > 16)
    {
        DpiLog(L"%s (count=%u monitors=%p -- skipped deref)", tag, count, monitors);
        return;
    }
    for (unsigned i = 0; i < count; ++i)
        DpiLog(L"%s mon[%u/%u] %s pos=(%ld,%ld) %lux%lu phys=%lux%lumm orient=%lu "
               L"DesktopScale=%lu%% DeviceScale=%lu%% flags=0x%08lX",
               tag, i + 1, count, (m[i].Flags & 1u) ? L"PRIMARY" : L"  2nd  ",
               static_cast<long>(m[i].Left), static_cast<long>(m[i].Top),
               static_cast<unsigned long>(m[i].Width), static_cast<unsigned long>(m[i].Height),
               static_cast<unsigned long>(m[i].PhysicalWidth),
               static_cast<unsigned long>(m[i].PhysicalHeight),
               static_cast<unsigned long>(m[i].Orientation),
               static_cast<unsigned long>(m[i].DesktopScaleFactor),
               static_cast<unsigned long>(m[i].DeviceScaleFactor),
               static_cast<unsigned long>(m[i].Flags));
}

// Rewrite the array in place before it ships (off by default; flip g_patchLayout).
static void MaybePatchLayout(unsigned int count, void* monitors)
{
    if (!g_patchLayout)
        return;
    DCML* m = reinterpret_cast<DCML*>(monitors);
    if (!m || count == 0 || count > 16)
        return;
    for (unsigned i = 0; i < count; ++i)
        (void)m[i];  // drop the fix here once the diff shows it, e.g. m[i].DeviceScaleFactor = 180;
}

static int Hooked_SendLayoutA(void* thisptr, unsigned int count, void* monitors)
{
    MaybePatchLayout(count, monitors);
    LogLayoutArray(L"SEND-A", count, monitors);
    return Real_SendLayoutA(thisptr, count, monitors);
}

static int Hooked_WriteLayout(void* thisptr, unsigned int count, void* monitors)
{
    // WriteMonitorLayoutPdu's array pointer has an 8-byte header; the DCML array starts at +8
    // (confirmed: WRITE fields lined up exactly +8 off VALIDATE's). Forward the original ptr.
    void* arr = monitors ? reinterpret_cast<BYTE*>(monitors) + 8 : nullptr;
    MaybePatchLayout(count, arr);
    LogLayoutArray(L"WRITE", count, arr);
    return Real_WriteLayout(thisptr, count, monitors);
}

static int Hooked_SendLayoutB(void* thisptr, unsigned int a, unsigned int b)
{
    DpiLog(L"SEND-B SendMonitorLayoutPdu(this=%p, a=%u, b=%u)", thisptr, a, b);
    return Real_SendLayoutB(thisptr, a, b);
}

static int Hooked_Validate(unsigned int count, void* monitors, void* arg3)
{
    LogLayoutArray(L"VALIDATE-in", count, monitors);
    int r = Real_Validate(count, monitors, arg3);
    DpiLog(L"VALIDATE -> returned %d (count=%u)", r, count);
    return r;
}

#pragma pack(push, 1)
struct GfxMon  // RdpGfxClientChannel::SetMonitorLayout input element: 20 bytes, 5 DWORDs (per disasm)
{
    LONG  Left;    // +0x00
    LONG  Top;     // +0x04
    DWORD Width;   // +0x08
    DWORD Height;  // +0x0c
    DWORD Field5;  // +0x10  -- unknown (flags? id? scale?); the value will tell us
};
#pragma pack(pop)
static_assert(sizeof(GfxMon) == 20, "GFX monitor element must be 20 bytes");

static int Hooked_GfxSetLayout(void* thisptr, unsigned int count, void* monitors, void* arg4)
{
    GfxMon* m = reinterpret_cast<GfxMon*>(monitors);
    if (m && count > 0 && count <= 16)
        for (unsigned i = 0; i < count; ++i)
            DpiLog(L"GFX layout[%u/%u] left=%ld top=%ld %lux%lu field5=%lu (0x%lX)",
                   i + 1, count, static_cast<long>(m[i].Left), static_cast<long>(m[i].Top),
                   static_cast<unsigned long>(m[i].Width), static_cast<unsigned long>(m[i].Height),
                   static_cast<unsigned long>(m[i].Field5), static_cast<unsigned long>(m[i].Field5));
    else
        DpiLog(L"GFX SetMonitorLayout(this=%p, count=%u, monitors=%p -- skipped)", thisptr, count, monitors);
    int r = Real_GfxSetLayout(thisptr, count, monitors, arg4);
    //TryForceSend(g_displayChannel, L"GFX");  // connect-time, fully laid out -> the "ready" moment to replay
    return r;
}

// The gate. mstsc asks "does the remote monitor layout already match local?" and SKIPS the
// RDPEDISP send when the answer is VARIANT_TRUE (-1) -- which is exactly why no layout (no
// scale) goes out at connect. We answer FALSE (0) on the 1st call so mstsc sends one; the
// truth thereafter. Logs the real value every call so we can see the timing if it misfires.
static LONG g_gateCalls = 0;
static HRESULT Hooked_MatchesLocal(void* thisptr, short* pVal)
{
    HRESULT hr = Real_MatchesLocal(thisptr, pVal);
    LONG n = InterlockedIncrement(&g_gateCalls);
    short real = (SUCCEEDED(hr) && pVal) ? *pVal : -2;
    const bool forced = (g_forceGate && n == 1 && SUCCEEDED(hr) && pVal);
    if (forced)
        *pVal = 0;  // VARIANT_FALSE: "does not match local" -> mstsc (re)sends the layout, scale and all
    DpiLog(L"GATE MatchesLocal call#%ld real=%d (-1=match,0=differ) hr=0x%08lX%s",
           n, static_cast<int>(real), static_cast<unsigned long>(hr), forced ? L" -> FORCED 0" : L"");
    return hr;
}

// FORCE-SEND: replay the dance's SendMonitorLayoutPdu(channel,0,0) exactly once, on the
// display channel's own thread the moment it's live. This is the call that ships the 200/180
// layout that mstsc otherwise only makes on a resize.
static LONG g_odrCalls = 0;
static void TryForceSend(void* channel, const wchar_t* from)
{
    if (!g_forceSend || !channel || !Real_SendLayoutB)
        return;
    if (g_forcedSend)
        return;  // a non-faulting attempt already completed
    DpiLog(L"FORCE-SEND: try SendMonitorLayoutPdu(this=%p, 0, 0) from %s", channel, from);
    int r = 0;
    __try
    {
        r = Real_SendLayoutB(channel, 0, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DpiLog(L"FORCE-SEND: FAULTED 0x%08lX from %s -- letting a later trigger point try",
               static_cast<unsigned long>(GetExceptionCode()), from);
        return;  // do NOT latch -> the next context may succeed
    }
    InterlockedExchange(&g_forcedSend, 1);  // latch only on a completed (non-faulting) call
    DpiLog(L"FORCE-SEND: returned %d from %s -- LATCHED (no fault)", r, from);
}

// Try the replay from each channel context, earliest first, all using the main-object 'this'
// captured by InitializeSelf (the corrected pointer). First non-faulting attempt wins.
static int Hooked_OnDataReceived(void* thisptr, void* a2, void* a3)
{
    int r = Real_OnDataReceived(thisptr, a2, a3);
    LONG n = InterlockedIncrement(&g_odrCalls);
    if (n <= 3)
        DpiLog(L"CHANNEL OnDataReceived this=%p call#%ld (main=%p)", thisptr, n, g_displayChannel);
    //TryForceSend(g_displayChannel, L"OnData");  // channel-ready context + corrected (main) 'this'
    TryForceSend((char*)thisptr + 8, L"OnData");  // channel-ready context + corrected (main) 'this'
    return r;
}

static int Hooked_InitSelf(void* thisptr, void* a2, void* a3)
{
    int r = Real_InitSelf(thisptr, a2, a3);
    g_displayChannel = thisptr;  // main-object 'this'
    DpiLog(L"CHANNEL InitializeSelf this=%p (captured)", thisptr);
    //TryForceSend(g_displayChannel, L"InitSelf");  // YOUR idea: from the channel's own init context, first
    return r;
}

// CodeView 'RSDS' debug record -> the module's PDB GUID, used to pin the exact build.
struct CvInfoRSDS { DWORD Sig; GUID Guid; DWORD Age; char Pdb[1]; };

static bool ModulePdbGuid(HMODULE mod, GUID* out)
{
    BYTE* base = reinterpret_cast<BYTE*>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    if (!d.VirtualAddress || d.Size < sizeof(IMAGE_DEBUG_DIRECTORY))
        return false;
    auto dbg = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(base + d.VirtualAddress);
    for (DWORD i = 0; i < d.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i)
    {
        if (dbg[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW && dbg[i].AddressOfRawData)
        {
            auto cv = reinterpret_cast<CvInfoRSDS*>(base + dbg[i].AddressOfRawData);
            if (cv->Sig == 0x53445352)  // 'RSDS'
            {
                *out = cv->Guid;
                return true;
            }
        }
    }
    return false;
}

struct KnownBuild
{
    GUID  guid;
    DWORD sendA;     // SendMonitorLayoutPdu(this,count,monitors)
    DWORD sendB;     // SendMonitorLayoutPdu(this,u32,u32)
    DWORD write;     // WriteMonitorLayoutPdu(this,count,monitors)
    DWORD validate;  // ValidateDisplayControlMonitorLayout(count,monitors,?)
    DWORD gfx;       // RdpGfxClientChannel::SetMonitorLayout(this,u32,ptr,?)
    DWORD gate;      // CMsTscAx::get_RemoteMonitorLayoutMatchesLocal(this,VARIANT_BOOL*)
    DWORD onData;    // RdpDisplayControlChannel::OnDataReceived(this,...)
    DWORD initSelf;  // RdpDisplayControlChannel::InitializeSelf(this,...)
};
static const KnownBuild kKnownBuilds[] = {
    // mstscax.dll PDB 6D429A36 (ships in 26100.8246 / .8328 / .8457 -- same binary)
    { { 0x6D429A36, 0x0C7A, 0xB9A0, { 0xA2, 0xBD, 0x12, 0x24, 0x07, 0xB8, 0xA7, 0x52 } },
      0x2AB0A0, 0x2AA760, 0x2AB2D4, 0x2A8630, 0x289D18, 0x3965C0, 0x2A9E90, 0x2A979C },
};

// Distinctive prologue of SendMonitorLayoutPdu:
//   mov rax,rsp; mov [rax+8],rbx; mov [rax+10],rbp; mov [rax+18],rsi; mov [rax+20],r14;
//   push r15; sub rsp,30h; mov r14,r8; mov r15d,edx; mov rbp,rcx
// Pure register ops (no relocated/absolute bytes) -> identical across patches, and the
// tail (r14<-r8, r15d<-edx, rbp<-rcx) *is* the (this,count,monitors) arg layout we rely
// on, so a byte-exact match self-validates it. Confirmed unique in mstscax 26100.8246.
static const BYTE kSendLayoutSig[] = {
    0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x48,0x89,0x68,0x10, 0x48,0x89,0x70,0x18,
    0x4C,0x89,0x70,0x20, 0x41,0x57, 0x48,0x83,0xEC,0x30, 0x4D,0x8B,0xF0, 0x44,0x8B,0xFA, 0x48,0x8B,0xE9
};

// Scan mstscax's executable sections for that prologue. Returns the address only if it
// occurs exactly once (0 -> refactored, >1 -> ambiguous) so we never hook blind.
static BYTE* FindSendLayoutBySig(HMODULE mod)
{
    BYTE* base = reinterpret_cast<BYTE*>(mod);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    BYTE* hit = nullptr;
    int n = 0;
    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s)
    {
        if (!(sec[s].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        BYTE* start = base + sec[s].VirtualAddress;
        DWORD size = sec[s].Misc.VirtualSize;
        for (DWORD i = 0; size >= sizeof(kSendLayoutSig) && i <= size - sizeof(kSendLayoutSig); ++i)
        {
            if (start[i] == kSendLayoutSig[0] &&
                memcmp(start + i, kSendLayoutSig, sizeof(kSendLayoutSig)) == 0)
            {
                hit = start + i;
                if (++n > 1)
                    return nullptr;  // ambiguous -> refuse
            }
        }
    }
    return (n == 1) ? hit : nullptr;
}

static LONG g_layoutInstalled = 0;

static bool InstallLayoutHook()
{
    if (g_layoutInstalled)
        return true;
    HMODULE mod = GetModuleHandleW(L"mstscax.dll");
    if (!mod)
        return false;  // mstscax not loaded yet -> keep polling
    if (InterlockedCompareExchange(&g_layoutInstalled, 1, 0) != 0)
        return true;

    GUID g{};
    const bool haveGuid = ModulePdbGuid(mod, &g);
    const KnownBuild* kb = nullptr;
    if (haveGuid)
        for (const KnownBuild& b : kKnownBuilds)
            if (IsEqualGUID(b.guid, g)) { kb = &b; break; }

    BYTE* base = reinterpret_cast<BYTE*>(mod);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (kb)
    {
        // Known build: hook the entire layout path so we catch the PDU wherever it flows.
        Real_SendLayoutA  = reinterpret_cast<SendLayoutArr_t>(base + kb->sendA);
        Real_SendLayoutB  = reinterpret_cast<SendLayoutWH_t>(base + kb->sendB);
        Real_WriteLayout  = reinterpret_cast<SendLayoutArr_t>(base + kb->write);
        Real_Validate     = reinterpret_cast<ValidateLayout_t>(base + kb->validate);
        Real_GfxSetLayout = reinterpret_cast<GfxSetLayout_t>(base + kb->gfx);
        Real_MatchesLocal = reinterpret_cast<GetMatchesLocal_t>(base + kb->gate);
        Real_OnDataReceived = reinterpret_cast<ChannelFn3_t>(base + kb->onData);
        Real_InitSelf       = reinterpret_cast<ChannelFn3_t>(base + kb->initSelf);
        LONG rA = DetourAttach(&reinterpret_cast<PVOID&>(Real_SendLayoutA),  Hooked_SendLayoutA);
        LONG rB = DetourAttach(&reinterpret_cast<PVOID&>(Real_SendLayoutB),  Hooked_SendLayoutB);
        LONG rW = DetourAttach(&reinterpret_cast<PVOID&>(Real_WriteLayout),  Hooked_WriteLayout);
        LONG rV = DetourAttach(&reinterpret_cast<PVOID&>(Real_Validate),     Hooked_Validate);
        LONG rG = DetourAttach(&reinterpret_cast<PVOID&>(Real_GfxSetLayout), Hooked_GfxSetLayout);
        LONG rGate = DetourAttach(&reinterpret_cast<PVOID&>(Real_MatchesLocal), Hooked_MatchesLocal);
        LONG rOdr = DetourAttach(&reinterpret_cast<PVOID&>(Real_OnDataReceived), Hooked_OnDataReceived);
        LONG rIni = DetourAttach(&reinterpret_cast<PVOID&>(Real_InitSelf),       Hooked_InitSelf);
        DpiLog(L"LAYOUT HOOK: build-table @ Send(wh)0x%lX Write0x%lX Validate0x%lX Gfx0x%lX Gate0x%lX OnData0x%lX Init0x%lX",
               kb->sendB, kb->write, kb->validate, kb->gfx, kb->gate, kb->onData, kb->initSelf);
        DpiLog(L"LAYOUT HOOK: attach (0=ok) Send(arr)=%ld Send(wh)=%ld Write=%ld Validate=%ld Gfx=%ld Gate=%ld OnData=%ld Init=%ld",
               rA, rB, rW, rV, rG, rGate, rOdr, rIni);
    }
    else
    {
        // Unknown build: locate at least Send(array) by its self-validating signature.
        BYTE* t = FindSendLayoutBySig(mod);
        if (t)
        {
            Real_SendLayoutA = reinterpret_cast<SendLayoutArr_t>(t);
            DetourAttach(&reinterpret_cast<PVOID&>(Real_SendLayoutA), Hooked_SendLayoutA);
            DpiLog(L"LAYOUT HOOK: Send(arr)@0x%lX via signature only "
                   L"(Validate/Write/Gfx need a build-table entry for this build)",
                   static_cast<unsigned long>(t - base));
        }
        else
            DpiLog(L"LAYOUT HOOK: nothing hooked -- unknown build "
                   L"guid={%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}; send me this line",
                   g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
                   g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    }
    LONG err = DetourTransactionCommit();
    DpiLog(L"LAYOUT HOOK: commit -> %ld", err);
    return true;
}

static void RemoveLayoutHook()
{
    if (!g_layoutInstalled)
        return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (Real_SendLayoutA)  DetourDetach(&reinterpret_cast<PVOID&>(Real_SendLayoutA),  Hooked_SendLayoutA);
    if (Real_SendLayoutB)  DetourDetach(&reinterpret_cast<PVOID&>(Real_SendLayoutB),  Hooked_SendLayoutB);
    if (Real_WriteLayout)  DetourDetach(&reinterpret_cast<PVOID&>(Real_WriteLayout),  Hooked_WriteLayout);
    if (Real_Validate)     DetourDetach(&reinterpret_cast<PVOID&>(Real_Validate),     Hooked_Validate);
    if (Real_GfxSetLayout) DetourDetach(&reinterpret_cast<PVOID&>(Real_GfxSetLayout), Hooked_GfxSetLayout);
    if (Real_MatchesLocal) DetourDetach(&reinterpret_cast<PVOID&>(Real_MatchesLocal), Hooked_MatchesLocal);
    if (Real_OnDataReceived) DetourDetach(&reinterpret_cast<PVOID&>(Real_OnDataReceived), Hooked_OnDataReceived);
    if (Real_InitSelf)       DetourDetach(&reinterpret_cast<PVOID&>(Real_InitSelf),       Hooked_InitSelf);
    DetourTransactionCommit();
}

static DWORD WINAPI LayoutHookPoll(LPVOID)
{
    // mstscax loads when the RDP control initializes -- a touch after our DLL lands but
    // before the first layout PDU. Poll briefly so we hook in time for the INITIAL send.
    for (int i = 0; i < 51200 && !InstallLayoutHook(); ++i)
        Sleep(1);
    return 0;
}

extern "C" __declspec(dllexport) LRESULT CALLBACK CBTProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && g_isMstsc)
    {
        EnsureHooksInstalled();
        EnsureDpiProbesInstalled();
        InstallLayoutHook();  // no-op until mstscax is loaded; backs up the poll thread
        mstsfence::DarkModeOnCbt(nCode, reinterpret_cast<HWND>(wParam));
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
            g_trace = GetEnvironmentVariableW(L"MSTSFENCE_TRACE", nullptr, 0) > 0;
            g_fenceEnabled = mstsfence::FenceEnabled();
            Log(L"ATTACH pid=%lu (mstsc) -- DLL is in. (trace=%d fence=%d)",
                GetCurrentProcessId(), g_trace, g_fenceEnabled);
            mstsfence::DarkModeOnAttach();
            // Race a poller against mstscax's load so the layout-send hook is in place
            // before the very first (initial-connect) PDU goes out.
            HANDLE th = CreateThread(nullptr, 0, LayoutHookPoll, nullptr, 0, nullptr);
            if (th) CloseHandle(th);
        }
        break;

    case DLL_PROCESS_DETACH:
        if (lpReserved == nullptr && g_isMstsc)
        {
            if (g_installed)
            {
                RemoveHooks();
            }
            if (g_dpiInstalled)
            {
                RemoveDpiProbes();
            }
            RemoveLayoutHook();
        }
        break;
    }
    return TRUE;
}
