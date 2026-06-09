// rdpedisp.cpp -- see rdpedisp.h. The connect-time DPI fix and the optional host-scale
// override, plus (behind MSTSFENCE_DPI_DIAG) the observe-hooks and DPI API probes that
// reverse-engineered it. Self-contained: its own logger, its own mstscax resolution;
// hook.cpp just calls OnAttach()/OnDetach().

#include <windows.h>
#include <shellscalingapi.h>   // GetDpiForMonitor / DEVICE_SCALE_FACTOR (only used by the diag probes)

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

#include "rdpedisp.h"
#include "settings.h"

namespace
{

// ---------------------------------------------------------------------------
// logging -- OutputDebugString + %TEMP%\mstsfencehook.log. Always on in a diag build (the
// whole point is to see it); otherwise gated on MSTSFENCE_TRACE so a shipping DLL is silent.

#define MSTSFENCE_DPI_DIAG 1

// ---------------------------------------------------------------------------
#ifdef MSTSFENCE_DPI_DIAG
constexpr bool kAlwaysLog = true;
#else
constexpr bool kAlwaysLog = false;
#endif

bool g_trace = false;  // MSTSFENCE_TRACE, read at OnAttach

void LogV(const wchar_t* fmt, va_list ap)
{
    wchar_t buf[1024];
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, ap);
    size_t len = wcsnlen(buf, ARRAYSIZE(buf));
    if (len == 0)
        return;
    OutputDebugStringW(buf);

    wchar_t path[MAX_PATH];
    DWORD t = GetTempPathW(ARRAYSIZE(path), path);
    if (t == 0 || t > ARRAYSIZE(path) || wcscat_s(path, L"mstsfencehook.log") != 0)
        return;
    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
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

void Log(const wchar_t* fmt, ...)
{
    if (!kAlwaysLog && !g_trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    LogV(fmt, ap);
    va_end(ap);
}

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
bool g_fixEnabled = false;       // HKCU\Software\mstsfence\DpiFix (default off): force the send at connect
bool g_overrideEnabled = false;  // HKCU\Software\mstsfence\DpiOverride: rewrite the host scale to a chosen %
unsigned g_overridePct = 100;    // HKCU\Software\mstsfence\DpiOverridePct: 100..500
LONG g_layoutInstalled = 0;      // channel hooks installed (once mstscax is in)
LONG g_forcedSend = 0;           // force-send latch -- re-armed per connection by InitSelf
BOOL g_fakeLayout = false;       // force flap by sending a fake and the correct layout
void* volatile g_displayChannel = nullptr;  // captured channel 'this' (diag/legacy; fix uses OnData+8)

using SendLayoutWH_t  = int (*)(void* thisptr, unsigned int a, unsigned int b);     // SendMonitorLayoutPdu(this,u32,u32)
using SendLayoutArr_t = int (*)(void* thisptr, unsigned int count, void* monitors); // Write / Send(array)
using ChannelFn3_t    = int (*)(void* thisptr, void* a2, void* a3);                 // OnDataReceived / InitializeSelf

SendLayoutWH_t  Real_SendLayoutB    = nullptr;  // the call the dance makes; we invoke its address directly
SendLayoutArr_t Real_WriteLayout    = nullptr;  // the serializer every send funnels through (override point)
ChannelFn3_t    Real_OnDataReceived = nullptr;
ChannelFn3_t    Real_InitSelf       = nullptr;
HMODULE(WINAPI* Real_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = LoadLibraryExW;

// ---------------------------------------------------------------------------
// build identity -- the fix/override only run on an mstscax whose PDB GUID we recognise, so
// the hard-coded RVAs (and the +8 'this' adjustment) are guaranteed correct for that binary.
// Unknown build -> safe no-op. Add a row (and confirm the offsets) for each new GUID.
// ---------------------------------------------------------------------------
struct CvInfoRSDS { DWORD Sig; GUID Guid; DWORD Age; char Pdb[1]; };

bool ModulePdbGuid(HMODULE mod, GUID* out)
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
    DWORD sendB;     // SendMonitorLayoutPdu(this,u32,u32)      -- the fix calls this
    DWORD onData;    // RdpDisplayControlChannel::OnDataReceived -- the fix hooks this (trigger)
    DWORD initSelf;  // RdpDisplayControlChannel::InitializeSelf -- the fix hooks this (re-arm)
    DWORD write;     // WriteMonitorLayoutPdu(this,count,monitors) -- the override hooks this
    DWORD sendA;     // SendMonitorLayoutPdu(this,count,monitors)        ] diag
    DWORD validate;  // ValidateDisplayControlMonitorLayout(count,...)   ] observe
    DWORD gfx;       // RdpGfxClientChannel::SetMonitorLayout(this,...)   ] hooks
    DWORD gate;      // CMsTscAx::get_RemoteMonitorLayoutMatchesLocal     ] only
};
const KnownBuild kKnownBuilds[] = {
    // mstscax.dll PDB 6D429A36 (ships in 26100.8246 / .8328 / .8457 -- same binary).
    // For this binary the send 'this' is OnDataReceived's 'this' + 8.
    { { 0x6D429A36, 0x0C7A, 0xB9A0, { 0xA2, 0xBD, 0x12, 0x24, 0x07, 0xB8, 0xA7, 0x52 } },
      0x2AA760, 0x2A9E90, 0x2A979C, 0x2AB2D4,
      0x2AB0A0, 0x2A8630, 0x289D18, 0x3965C0 },
};

// ---------------------------------------------------------------------------
// the layout struct + the optional host-scale override (always compiled)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct DCML  // DISPLAYCONTROL_MONITOR_LAYOUT, 40 bytes; offsets confirmed from the validator disasm
{
    DWORD Flags; LONG Left; LONG Top; DWORD Width; DWORD Height;
    DWORD PhysicalWidth; DWORD PhysicalHeight; DWORD Orientation;
    DWORD DesktopScaleFactor; DWORD DeviceScaleFactor;
};
#pragma pack(pop)
static_assert(sizeof(DCML) == 40, "DISPLAYCONTROL_MONITOR_LAYOUT must be 40 bytes");

// DeviceScaleFactor must be 100, 140, or 180; snap the desktop scale to the nearest tier.
DWORD DeviceScaleTierFor(unsigned int pct)
{
    if (pct >= 160) return 180;
    if (pct >= 120) return 140;
    if (pct >= 100) return 100;
    return 0;
}

// Rewrite every monitor's scale to the configured % (clamped 100..500) before it ships.
void ApplyOverride(unsigned int count, void* dcmlArray)
{
    if (!g_overrideEnabled || !dcmlArray || count == 0 || count > 16)
        return;

    auto overridePct = g_overridePct;
    if (g_fakeLayout)
    {
        if (overridePct == 100)
            overridePct = 200;
        else
            overridePct = 100;
    }

    DWORD device = DeviceScaleTierFor(overridePct);
    DCML* m = reinterpret_cast<DCML*>(dcmlArray);
    for (unsigned i = 0; i < count; ++i)
    {
        m[i].DesktopScaleFactor = overridePct;
        m[i].DeviceScaleFactor = device;
    }
}

#ifdef MSTSFENCE_DPI_DIAG
// (declared early so the always-compiled Hooked_WriteLayout below can log through it)
void LogLayoutArray(const wchar_t* tag, unsigned int count, void* monitors)
{
    DCML* m = reinterpret_cast<DCML*>(monitors);
    if (!m || count == 0 || count > 16)
    {
        Log(L"%s (count=%u monitors=%p -- skipped deref)", tag, count, monitors);
        return;
    }
    for (unsigned i = 0; i < count; ++i)
        Log(L"%s mon[%u/%u] %s pos=(%ld,%ld) %lux%lu phys=%lux%lumm orient=%lu "
            L"DesktopScale=%lu%% DeviceScale=%lu%% flags=0x%08lX",
            tag, i + 1, count, (m[i].Flags & 1u) ? L"PRIMARY" : L"  2nd  ",
            static_cast<long>(m[i].Left), static_cast<long>(m[i].Top),
            static_cast<unsigned long>(m[i].Width), static_cast<unsigned long>(m[i].Height),
            static_cast<unsigned long>(m[i].PhysicalWidth), static_cast<unsigned long>(m[i].PhysicalHeight),
            static_cast<unsigned long>(m[i].Orientation),
            static_cast<unsigned long>(m[i].DesktopScaleFactor),
            static_cast<unsigned long>(m[i].DeviceScaleFactor),
            static_cast<unsigned long>(m[i].Flags));
}
#endif

// WriteMonitorLayoutPdu(this, count, monitors): the array sits at monitors+8 (an 8-byte
// header). We rewrite the scale here (the override) just before the bytes are serialized.
int Hooked_WriteLayout(void* thisptr, unsigned int count, void* monitors)
{
    // if override % is zero, block sending any calls
    if (g_overridePct == 0)
    {
        Log(L"WRITE bailing out");
        return 0;
    }
    void* arr = monitors ? reinterpret_cast<BYTE*>(monitors) + 8 : nullptr;
    ApplyOverride(count, arr);
#ifdef MSTSFENCE_DPI_DIAG
    LogLayoutArray(L"WRITE", count, arr);
#endif
    return Real_WriteLayout(thisptr, count, monitors);
}

// ---------------------------------------------------------------------------
// diagnostics (only under MSTSFENCE_DPI_DIAG): observe hooks on the rest of the layout path
// + the user32/gdi32/shcore DPI-read probes. Not needed for the fix or the override.
// ---------------------------------------------------------------------------
#ifdef MSTSFENCE_DPI_DIAG

bool g_logApiProbes = false;  // the API probes are chatty -> off even in a diag build unless flipped
bool g_forceGate    = true;   // answer get_RemoteMonitorLayoutMatchesLocal FALSE once

#pragma pack(push, 1)
struct GfxMon { LONG Left; LONG Top; DWORD Width; DWORD Height; DWORD Field5; };  // GFX element, 20 bytes
#pragma pack(pop)
static_assert(sizeof(GfxMon) == 20, "GFX monitor element must be 20 bytes");

using ValidateLayout_t  = int (*)(unsigned int count, void* monitors, void* arg3);
using GfxSetLayout_t    = int (*)(void* thisptr, unsigned int a, void* ptr, void* arg4);
using GetMatchesLocal_t = HRESULT (*)(void* thisptr, short* pVal);

SendLayoutArr_t  Real_SendLayoutA  = nullptr;
ValidateLayout_t Real_Validate     = nullptr;
GfxSetLayout_t   Real_GfxSetLayout = nullptr;
GetMatchesLocal_t Real_MatchesLocal = nullptr;

UINT(WINAPI* Real_GetDpiForSystem)(void) = GetDpiForSystem;
UINT(WINAPI* Real_GetDpiForWindow)(HWND) = GetDpiForWindow;
int(WINAPI* Real_GetDeviceCaps)(HDC, int) = GetDeviceCaps;
HRESULT(WINAPI* Real_GetDpiForMonitor)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*) = GetDpiForMonitor;
HRESULT(WINAPI* Real_GetScaleFactorForMonitor)(HMONITOR, DEVICE_SCALE_FACTOR*) = GetScaleFactorForMonitor;
LONG g_apiInstalled = 0;
LONG g_gateCalls = 0;

int Hooked_SendLayoutA(void* thisptr, unsigned int count, void* monitors)
{
    LogLayoutArray(L"SEND-A", count, monitors);
    return Real_SendLayoutA(thisptr, count, monitors);
}
int Hooked_SendLayoutB(void* thisptr, unsigned int a, unsigned int b)
{
    Log(L"SEND-B SendMonitorLayoutPdu(this=%p, a=%u, b=%u)", thisptr, a, b);
    return Real_SendLayoutB(thisptr, a, b);
}
int Hooked_Validate(unsigned int count, void* monitors, void* arg3)
{
    LogLayoutArray(L"VALIDATE-in", count, monitors);
    int r = Real_Validate(count, monitors, arg3);
    Log(L"VALIDATE -> returned %d (count=%u)", r, count);
    return r;
}
int Hooked_GfxSetLayout(void* thisptr, unsigned int count, void* monitors, void* arg4)
{
    GfxMon* m = reinterpret_cast<GfxMon*>(monitors);
    if (m && count > 0 && count <= 16)
        for (unsigned i = 0; i < count; ++i)
            Log(L"GFX layout[%u/%u] left=%ld top=%ld %lux%lu field5=%lu (0x%lX)",
                i + 1, count, static_cast<long>(m[i].Left), static_cast<long>(m[i].Top),
                static_cast<unsigned long>(m[i].Width), static_cast<unsigned long>(m[i].Height),
                static_cast<unsigned long>(m[i].Field5), static_cast<unsigned long>(m[i].Field5));
    else
        Log(L"GFX SetMonitorLayout(this=%p, count=%u, monitors=%p -- skipped)", thisptr, count, monitors);
    return Real_GfxSetLayout(thisptr, count, monitors, arg4);
}
HRESULT Hooked_MatchesLocal(void* thisptr, short* pVal)
{
    HRESULT hr = Real_MatchesLocal(thisptr, pVal);
    LONG n = InterlockedIncrement(&g_gateCalls);
    short real = (SUCCEEDED(hr) && pVal) ? *pVal : -2;
    const bool forced = (g_forceGate && n == 1 && SUCCEEDED(hr) && pVal);
    if (forced)
        *pVal = 0;  // VARIANT_FALSE
    Log(L"GATE MatchesLocal call#%ld real=%d (-1=match,0=differ) hr=0x%08lX%s",
        n, static_cast<int>(real), static_cast<unsigned long>(hr), forced ? L" -> FORCED 0" : L"");
    return hr;
}

UINT WINAPI Hooked_GetDpiForSystem(void)
{
    UINT dpi = Real_GetDpiForSystem();
    if (g_logApiProbes) Log(L"DPI probe: GetDpiForSystem() -> %u (%d%%)", dpi, MulDiv(static_cast<int>(dpi), 100, 96));
    return dpi;
}
UINT WINAPI Hooked_GetDpiForWindow(HWND hwnd)
{
    UINT dpi = Real_GetDpiForWindow(hwnd);
    if (g_logApiProbes) Log(L"DPI probe: GetDpiForWindow(hwnd=%p) -> %u (%d%%)", hwnd, dpi, MulDiv(static_cast<int>(dpi), 100, 96));
    return dpi;
}
int WINAPI Hooked_GetDeviceCaps(HDC hdc, int index)
{
    int v = Real_GetDeviceCaps(hdc, index);
    if (g_logApiProbes && index == LOGPIXELSX)
        Log(L"DPI probe: GetDeviceCaps(hdc=%p, LOGPIXELSX) -> %d (%d%%)", hdc, v, MulDiv(v, 100, 96));
    return v;
}
HRESULT WINAPI Hooked_GetDpiForMonitor(HMONITOR hmon, MONITOR_DPI_TYPE type, UINT* dpiX, UINT* dpiY)
{
    HRESULT hr = Real_GetDpiForMonitor(hmon, type, dpiX, dpiY);
    if (g_logApiProbes)
    {
        UINT x = (SUCCEEDED(hr) && dpiX) ? *dpiX : 0u, y = (SUCCEEDED(hr) && dpiY) ? *dpiY : 0u;
        Log(L"DPI probe: GetDpiForMonitor(hMon=%p, type=%d) -> hr=0x%08lX dpiX=%u dpiY=%u",
            hmon, static_cast<int>(type), static_cast<unsigned long>(hr), x, y);
    }
    return hr;
}
HRESULT WINAPI Hooked_GetScaleFactorForMonitor(HMONITOR hmon, DEVICE_SCALE_FACTOR* scale)
{
    HRESULT hr = Real_GetScaleFactorForMonitor(hmon, scale);
    if (g_logApiProbes)
    {
        int s = (SUCCEEDED(hr) && scale) ? static_cast<int>(*scale) : 0;
        Log(L"DPI probe: GetScaleFactorForMonitor(hMon=%p) -> hr=0x%08lX scale=%d%%",
            hmon, static_cast<unsigned long>(hr), s);
    }
    return hr;
}

void InstallApiProbes()
{
    if (InterlockedCompareExchange(&g_apiInstalled, 1, 0) != 0)
        return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetDpiForSystem), Hooked_GetDpiForSystem);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetDpiForWindow), Hooked_GetDpiForWindow);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetDeviceCaps), Hooked_GetDeviceCaps);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetDpiForMonitor), Hooked_GetDpiForMonitor);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GetScaleFactorForMonitor), Hooked_GetScaleFactorForMonitor);
    DetourTransactionCommit();
}
void RemoveApiProbes()
{
    if (!g_apiInstalled)
        return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetDpiForSystem), Hooked_GetDpiForSystem);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetDpiForWindow), Hooked_GetDpiForWindow);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetDeviceCaps), Hooked_GetDeviceCaps);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetDpiForMonitor), Hooked_GetDpiForMonitor);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_GetScaleFactorForMonitor), Hooked_GetScaleFactorForMonitor);
    DetourTransactionCommit();
}

#endif  // MSTSFENCE_DPI_DIAG

// ---------------------------------------------------------------------------
// the connect-time fix
// ---------------------------------------------------------------------------
// Replay the dance's SendMonitorLayoutPdu(channel, 0, 0) exactly once per connection, on the
// channel's own (OnDataReceived) thread. SEH-guarded: a wrong this/state degrades to a logged
// fault instead of crashing mstsc -- though on a GUID-matched build it does not fault.
void TryForceSend(void* channel)
{
    if (!g_fixEnabled || !channel || !Real_SendLayoutB)
        return;
    if (g_forcedSend)
        return;  // already sent for this connection (re-armed by InitSelf on reconnect)
    int r = 0;
    Log(L"rdpedisp: TryForceSend (this=%p) -> %d", channel, r);
    __try
    {
        g_fakeLayout = true;
        r = Real_SendLayoutB(channel, 0, 0);
        Log(L"rdpedisp: forced monitor-layout send at connect (this=%p, fake: %d) -> %d", channel, g_fakeLayout, r);
        g_fakeLayout = false;
        r = Real_SendLayoutB(channel, 0, 0);
        Log(L"rdpedisp: forced monitor-layout send at connect (this=%p, fake: %d) -> %d", channel, g_fakeLayout, r);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(L"rdpedisp: forced send FAULTED 0x%08lX (this=%p) -- mstsc kept alive",
            static_cast<unsigned long>(GetExceptionCode()), channel);
        return;
    }
    InterlockedExchange(&g_forcedSend, 1);
}

int Hooked_OnDataReceived(void* thisptr, void* a2, void* a3)
{
    int r = Real_OnDataReceived(thisptr, a2, a3);
    // The display-control interface sub-object that owns SendMonitorLayoutPdu sits +8 from
    // the IWTSVirtualChannelCallback sub-object that OnDataReceived runs on (multiple
    // inheritance; offset pinned to the known build).
    Log(L"rdpedisp: OnDataReceived this=%p", thisptr);
    TryForceSend(reinterpret_cast<BYTE*>(thisptr) + 8);
    return r;
}

int Hooked_InitSelf(void* thisptr, void* a2, void* a3)
{
    int r = Real_InitSelf(thisptr, a2, a3);
    g_displayChannel = thisptr;
    InterlockedExchange(&g_forcedSend, 0);  // new connection -> allow the force again
    Log(L"rdpedisp: InitSelf this=%p (re-armed)", thisptr);
    return r;
}

bool InstallChannelHooks()
{
    if (g_layoutInstalled)
        return true;
    HMODULE mod = GetModuleHandleW(L"mstscax.dll");
    if (!mod)
        return false;  // not loaded yet
    if (InterlockedCompareExchange(&g_layoutInstalled, 1, 0) != 0)
        return true;

    GUID g{};
    const KnownBuild* kb = nullptr;
    if (ModulePdbGuid(mod, &g))
        for (const KnownBuild& b : kKnownBuilds)
            if (IsEqualGUID(b.guid, g)) { kb = &b; break; }
    if (!kb)
    {
        Log(L"rdpedisp: unrecognized mstscax build "
            L"guid={%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X} -- DPI fix/override not applied; send me this line",
            g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        return true;  // safe no-op
    }

    BYTE* base = reinterpret_cast<BYTE*>(mod);
    Real_SendLayoutB    = reinterpret_cast<SendLayoutWH_t>(base + kb->sendB);   // address to call (not detoured)
    Real_OnDataReceived = reinterpret_cast<ChannelFn3_t>(base + kb->onData);
    Real_InitSelf       = reinterpret_cast<ChannelFn3_t>(base + kb->initSelf);

    // The serializer is only hooked when the override is on (or in a diag build to observe).
#ifdef MSTSFENCE_DPI_DIAG
    bool wantWrite = true;
#else
    bool wantWrite = g_overrideEnabled;
#endif

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(Real_OnDataReceived), Hooked_OnDataReceived);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_InitSelf), Hooked_InitSelf);
    if (wantWrite)
    {
        Real_WriteLayout = reinterpret_cast<SendLayoutArr_t>(base + kb->write);
        DetourAttach(&reinterpret_cast<PVOID&>(Real_WriteLayout), Hooked_WriteLayout);
    }
#ifdef MSTSFENCE_DPI_DIAG
    Real_SendLayoutA  = reinterpret_cast<SendLayoutArr_t>(base + kb->sendA);
    Real_Validate     = reinterpret_cast<ValidateLayout_t>(base + kb->validate);
    Real_GfxSetLayout = reinterpret_cast<GfxSetLayout_t>(base + kb->gfx);
    Real_MatchesLocal = reinterpret_cast<GetMatchesLocal_t>(base + kb->gate);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_SendLayoutA), Hooked_SendLayoutA);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_Validate), Hooked_Validate);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_GfxSetLayout), Hooked_GfxSetLayout);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_MatchesLocal), Hooked_MatchesLocal);
    DetourAttach(&reinterpret_cast<PVOID&>(Real_SendLayoutB), Hooked_SendLayoutB);  // observe SendB (-> trampoline)
#endif
    LONG err = DetourTransactionCommit();
    Log(L"rdpedisp: channel hooks installed (build matched); commit=%ld fix=%d override=%d(%u%%)",
        err, g_fixEnabled, g_overrideEnabled, g_overridePct);
    return true;
}

void RemoveChannelHooks()
{
    if (!g_layoutInstalled)
        return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (Real_OnDataReceived) DetourDetach(&reinterpret_cast<PVOID&>(Real_OnDataReceived), Hooked_OnDataReceived);
    if (Real_InitSelf)       DetourDetach(&reinterpret_cast<PVOID&>(Real_InitSelf), Hooked_InitSelf);
    if (Real_WriteLayout)    DetourDetach(&reinterpret_cast<PVOID&>(Real_WriteLayout), Hooked_WriteLayout);
#ifdef MSTSFENCE_DPI_DIAG
    if (Real_SendLayoutA)  DetourDetach(&reinterpret_cast<PVOID&>(Real_SendLayoutA), Hooked_SendLayoutA);
    if (Real_Validate)     DetourDetach(&reinterpret_cast<PVOID&>(Real_Validate), Hooked_Validate);
    if (Real_GfxSetLayout) DetourDetach(&reinterpret_cast<PVOID&>(Real_GfxSetLayout), Hooked_GfxSetLayout);
    if (Real_MatchesLocal) DetourDetach(&reinterpret_cast<PVOID&>(Real_MatchesLocal), Hooked_MatchesLocal);
    DetourDetach(&reinterpret_cast<PVOID&>(Real_SendLayoutB), Hooked_SendLayoutB);
#endif
    DetourTransactionCommit();
}

// mstscax loads when the RDP control is instantiated (via COM -> LoadLibraryEx), after our DLL
// is already in. We catch that load here and install the channel hooks right after it maps --
// running at the return of LoadLibraryExW, so the loader lock is already released. No poll.
HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    HMODULE h = Real_LoadLibraryExW(name, file, flags);
    if (!g_layoutInstalled && GetModuleHandleW(L"mstscax.dll"))
        InstallChannelHooks();
    return h;
}

LONG g_loadHook = 0;

}  // namespace

// ---------------------------------------------------------------------------
// public entry points
// ---------------------------------------------------------------------------
void mstsfence::rdpedisp::OnAttach()
{
    g_trace = GetEnvironmentVariableW(L"MSTSFENCE_TRACE", nullptr, 0) > 0;
    g_fixEnabled = mstsfence::DpiFixEnabled();
    g_overrideEnabled = mstsfence::DpiOverrideEnabled();
    g_overridePct = mstsfence::DpiOverridePct();

#ifdef MSTSFENCE_DPI_DIAG
    InstallApiProbes();   // observe-only DPI reads; always hook mstscax too (we want the data)
#else
    if (!g_fixEnabled && !g_overrideEnabled)
    {
        Log(L"rdpedisp: idle (DpiFix + DpiOverride both off).");
        return;  // nothing to hook
    }
#endif

    // Hook LoadLibraryExW so we catch mstscax whenever it loads, then immediately cover the
    // case where it's already in (we're injected via CBT -- the order is a coin flip).
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(Real_LoadLibraryExW), Hooked_LoadLibraryExW);
    if (DetourTransactionCommit() == NO_ERROR)
        InterlockedExchange(&g_loadHook, 1);
    if (GetModuleHandleW(L"mstscax.dll"))
        InstallChannelHooks();
}

void mstsfence::rdpedisp::OnDetach()
{
    if (g_loadHook)
    {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&reinterpret_cast<PVOID&>(Real_LoadLibraryExW), Hooked_LoadLibraryExW);
        DetourTransactionCommit();
    }
    RemoveChannelHooks();
#ifdef MSTSFENCE_DPI_DIAG
    RemoveApiProbes();
#endif
}
