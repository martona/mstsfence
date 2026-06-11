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
unsigned g_nudgePx = 0;          // HKCU\Software\mstsfence\DpiNudge: shave N px off the advertised primary
                                 // height at connect to force the host to (re)open display control (0 = off)
unsigned g_injectScale = 0;      // HKCU\Software\mstsfence\DpiInjectScale: synthesize a CS_MONITOR_EX with
                                 // this DesktopScaleFactor at connect (single-monitor) -- experiment (0 = off)
LONG g_connectW = 0;             // desktop dims captured from OnInitiateConnection (CS_CORE), for the inject
LONG g_connectH = 0;
LONG g_layoutInstalled = 0;      // channel hooks installed (once mstscax is in)
LONG g_forcedSend = 0;           // force-send latch -- re-armed per connection by InitSelf
BOOL g_fakeLayout = false;       // force flap by sending a fake and the correct layout
void* volatile g_displayChannel = nullptr;  // captured channel 'this' (diag/legacy; fix uses OnData+8)

using SendLayoutWH_t  = int (*)(void* thisptr, unsigned int a, unsigned int b);     // SendMonitorLayoutPdu(this,u32,u32)
using SendLayoutArr_t = int (*)(void* thisptr, unsigned int count, void* monitors); // Write / Send(array)
using ChannelFn3_t    = int (*)(void* thisptr, void* a2, void* a3);                 // OnDataReceived / InitializeSelf
// CNC::NC_GetMONITORData(this, csMonitorHdr, monitorDefArray, +7 more) -- the connect-time CS_MONITOR
// builder. 10 args (rcx,rdx,r8,r9 + 6 on the stack); the full signature must match so the trampoline
// gets every arg. We only read arg2 (CS_MONITOR header) and arg3 (the DEF array).
using GetMonData_t    = int (*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
// CoreFSM::OnInitiateConnection(this, params, +2 unused) -- builds the CS_CORE block. Uses only rcx/rdx
// (r8/r9 are clobbered before any read, no stack args), so a 4-arg pass-through trampoline is safe.
using OnInitConn_t    = int (*)(void*, void*, void*, void*);

SendLayoutWH_t  Real_SendLayoutB    = nullptr;  // the call the dance makes; we invoke its address directly
SendLayoutArr_t Real_WriteLayout    = nullptr;  // the serializer every send funnels through (override point)
ChannelFn3_t    Real_OnDataReceived = nullptr;
ChannelFn3_t    Real_InitSelf       = nullptr;
GetMonData_t    Real_GetMonData     = nullptr;  // connect-time CS_MONITOR builder (multimon nudge)
OnInitConn_t    Real_OnInit         = nullptr;  // connect-time CS_CORE builder (the primary nudge)
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
    DWORD sendB;       // SendMonitorLayoutPdu(this,u32,u32)      -- the fix calls this
    DWORD onData;      // RdpDisplayControlChannel::OnDataReceived -- the fix hooks this (trigger)
    DWORD initSelf;    // RdpDisplayControlChannel::InitializeSelf -- the fix hooks this (re-arm)
    DWORD write;       // WriteMonitorLayoutPdu(this,count,monitors) -- the override hooks this
    DWORD onInit;      // CoreFSM::OnInitiateConnection -- builds CS_CORE; the nudge shaves desktopHeight here
    DWORD getMonData;  // CNC::NC_GetMONITORData -- builds CS_MONITOR (multimon only); nudge shaves it too
    DWORD sendA;       // SendMonitorLayoutPdu(this,count,monitors)        ] diag
    DWORD validate;    // ValidateDisplayControlMonitorLayout(count,...)   ] observe
    DWORD gfx;         // RdpGfxClientChannel::SetMonitorLayout(this,...)   ] hooks
    DWORD gate;        // CMsTscAx::get_RemoteMonitorLayoutMatchesLocal     ] only
};
const KnownBuild kKnownBuilds[] = {
    // mstscax.dll PDB 6D429A36 (ships in 26100.8246 / .8328 / .8457 -- same binary).
    // For this binary the send 'this' is OnDataReceived's 'this' + 8.
    { { 0x6D429A36, 0x0C7A, 0xB9A0, { 0xA2, 0xBD, 0x12, 0x24, 0x07, 0xB8, 0xA7, 0x52 } },
      0x2AA760, 0x2A9E90, 0x2A979C, 0x2AB2D4, 0x13EE24, 0x15F5B0,
      0x2AB0A0, 0x2A8630, 0x289D18, 0x3965C0 },
};

// ---------------------------------------------------------------------------
// the layout struct + the optional host-scale override (always compiled)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct DCML  // DISPLAYCONTROL_MONITOR_LAYOUT, 40 bytes; offsets confirmed from the validator disasm.
             // Flags: bit0 = DISPLAYCONTROL_MONITOR_PRIMARY (the only documented one; single monitor =>
             // always 1); bit1 = skip this monitor in the adjacency/overlap check (mstsc-internal, undoc).
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
        #if 0 // this is temporarily (?) off - it does not seem to have the desired effect
        g_fakeLayout = true;
        r = Real_SendLayoutB(channel, 0, 0);
        Log(L"rdpedisp: forced monitor-layout send at connect (this=%p, fake: %d) -> %d", channel, g_fakeLayout, r);
        g_fakeLayout = false;
        #endif
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

// ---------------------------------------------------------------------------
// the connect-time resolution nudge (the "dark channel" workaround)
// ---------------------------------------------------------------------------
// When you reconnect to an *already-logged-on* session, the host reuses the session's existing
// display config and usually never (re)opens the display-control channel -- so OnDataReceived
// never fires and the fix above has nothing to hook (host stuck at the wrong scale). The lever:
// make the advertised resolution differ from the session's current one, so the host is forced to
// reconfigure the display -- which (re)opens display control. Then the force-send above applies the
// true scale. We shave g_nudgePx off the advertised primary-monitor height at connect; the force-send
// reads a different source (RdpWinMonitorConfig), so it still sends the true 4K once the channel opens.
//
// There are two blocks carrying the size, depending on the connection:
//   * CS_CORE (always sent) -- desktopHeight. Built by CoreFSM::OnInitiateConnection; this is the
//     ONLY size a single-monitor connect advertises (CS_MONITOR is gated on UseMultimon). PRIMARY hook.
//   * CS_MONITOR (multimon only) -- the per-monitor rectangles. Built by CNC::NC_GetMONITORData.
//
// CS_MONITOR path: by the time NC_GetMONITORData returns,
//   arg2 (csMonHdr) = CS_MONITOR header { WORD type=0xC005, WORD len, DWORD flags, DWORD count }
//   arg3 (defArray) = count x TS_MONITOR_DEF { LONG left, top, right, bottom; DWORD flags } (20B each)
// Primary monitor has flags bit 0. Modifying a coordinate is length-safe (no header length changes).
// On a single monitor UseMultimon=0 -> the block is never emitted (*arg2 stays 0) -> we no-op quietly.
int Hooked_GetMonData(void* a1, void* csMonHdr, void* defArray, void* a4, void* a5,
                      void* a6, void* a7, void* a8, void* a9, void* a10)
{
    int r = Real_GetMonData(a1, csMonHdr, defArray, a4, a5, a6, a7, a8, a9, a10);
#ifdef MSTSFENCE_DPI_DIAG
    // OBSERVE what the connect advertises. arg2 = CS_MONITOR header (0xC005); arg5 = CS_MONITOR_EX header
    // (0xC008) { type,len,flags, monitorAttributeSize@8, count@0xC }; arg6 = the attributes array that
    // follows it (TS_MONITOR_ATTRIBUTES, 20B: physW, physH, orientation, DesktopScaleFactor@0xC,
    // DeviceScaleFactor@0x10). Tag 0x0000 => the block was NOT emitted (single monitor / UseMultimon off)
    // => the client advertises NO scale at connect, so the host has only its default (100%) to fall back on.
    __try
    {
        WORD csm = csMonHdr ? *reinterpret_cast<WORD*>(csMonHdr) : 0;
        WORD cse = a5 ? *reinterpret_cast<WORD*>(a5) : 0;
        Log(L"GETMON: r=%d CS_MONITOR=0x%04X CS_MONITOR_EX=0x%04X%s", r, csm, cse,
            (csm != 0xC005 && cse != 0xC008) ? L"  (nothing emitted -> no connect-time scale advertised)" : L"");
        if (cse == 0xC008 && a6)
        {
            DWORD exCount = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(a5) + 0xC);
            if (exCount >= 1 && exCount <= 16)
                for (DWORD i = 0; i < exCount; ++i)
                {
                    BYTE* at = static_cast<BYTE*>(a6) + i * 20;
                    Log(L"GETMON:   MON_EX[%lu] physW=%lumm physH=%lumm orient=%lu DesktopScale=%lu%% DeviceScale=%lu%%",
                        i, *reinterpret_cast<DWORD*>(at + 0), *reinterpret_cast<DWORD*>(at + 4),
                        *reinterpret_cast<DWORD*>(at + 8), *reinterpret_cast<DWORD*>(at + 0xC),
                        *reinterpret_cast<DWORD*>(at + 0x10));
                }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log(L"GETMON: observe faulted"); }
#endif
    // EXPERIMENT: when the real builder skipped the monitor blocks (single monitor, *csMonHdr != 0xC005),
    // synthesize a 1-monitor CS_MONITOR + CS_MONITOR_EX carrying DesktopScaleFactor=g_injectScale, to test
    // whether the host honors a *connect-time* scale (which would fix fresh AND reconnect). Every write
    // lands in PrepareGccUserData's own frame locals: csMonHdr=a2 header, defArray=a3, a5=CS_MONITOR_EX
    // header, a6=TS_MONITOR_ATTRIBUTES, a4=CS_MONITOR length out, a7=CS_MONITOR_EX length out.
    // a8/a9/a10 are for a separate 0xC00B monitor block, so leave them untouched.
    if (g_injectScale > 0 && csMonHdr && defArray && a5 && a6 && a4 && a7)
    {
        __try
        {
            if (*reinterpret_cast<WORD*>(csMonHdr) != 0xC005)  // real builder emitted nothing -> ours
            {
                LONG  W = g_connectW > 0 ? g_connectW : 1920;
                LONG  H = g_connectH > 0 ? g_connectH : 1080;
                DWORD desk = g_injectScale, dev = DeviceScaleTierFor(g_injectScale);
                DWORD dpi  = 96u * desk / 100u;                       // physical size, kept consistent with the scale
                DWORD physW = dpi ? static_cast<DWORD>(static_cast<long long>(W) * 254 / (dpi * 10)) : 520;
                DWORD physH = dpi ? static_cast<DWORD>(static_cast<long long>(H) * 254 / (dpi * 10)) : 290;

                BYTE* m = static_cast<BYTE*>(csMonHdr);              // CS_MONITOR header (12B) + DEF
                *reinterpret_cast<WORD*>(m)       = 0xC005;
                *reinterpret_cast<WORD*>(m + 2)   = 12 + 20;          // TS_UD length
                *reinterpret_cast<DWORD*>(m + 4)  = 0;                // flags
                *reinterpret_cast<DWORD*>(m + 8)  = 1;                // monitorCount
                BYTE* d = static_cast<BYTE*>(defArray);              // TS_MONITOR_DEF { l, t, inclusive-r, inclusive-b, flags }
                *reinterpret_cast<LONG*>(d)       = 0;
                *reinterpret_cast<LONG*>(d + 4)   = 0;
                *reinterpret_cast<LONG*>(d + 8)   = W - 1;
                *reinterpret_cast<LONG*>(d + 12)  = H - 1;
                *reinterpret_cast<DWORD*>(d + 16) = 1;                // TS_MONITOR_PRIMARY

                BYTE* e = static_cast<BYTE*>(a5);                    // CS_MONITOR_EX header (16B)
                *reinterpret_cast<WORD*>(e)       = 0xC008;
                *reinterpret_cast<WORD*>(e + 2)   = 16 + 20;          // TS_UD length
                *reinterpret_cast<DWORD*>(e + 4)  = 0;                // flags
                *reinterpret_cast<DWORD*>(e + 8)  = 20;               // monitorAttributeSize
                *reinterpret_cast<DWORD*>(e + 0xC) = 1;              // monitorCount
                BYTE* at = static_cast<BYTE*>(a6);                  // TS_MONITOR_ATTRIBUTES
                *reinterpret_cast<DWORD*>(at)       = physW;
                *reinterpret_cast<DWORD*>(at + 4)   = physH;
                *reinterpret_cast<DWORD*>(at + 8)   = 0;             // orientation (landscape)
                *reinterpret_cast<DWORD*>(at + 0xC) = desk;          // DesktopScaleFactor
                *reinterpret_cast<DWORD*>(at + 0x10) = dev;          // DeviceScaleFactor

                *reinterpret_cast<DWORD*>(a4) = 12 + 20;              // CS_MONITOR length out-param
                *reinterpret_cast<DWORD*>(a7) = 16 + 20;              // CS_MONITOR_EX length out-param
                Log(L"rdpedisp: INJECTED CS_MONITOR+EX (1 mon %ldx%ld phys=%lux%lumm DesktopScale=%lu%% DeviceScale=%lu%%) at connect",
                    W, H, physW, physH, desk, dev);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log(L"rdpedisp: inject FAULTED 0x%08lX -- connect left as-is", static_cast<unsigned long>(GetExceptionCode()));
        }
    }
    if (g_nudgePx == 0 || r < 0 || !csMonHdr || !defArray)
        return r;  // nudge off, or the builder failed -> leave the connect untouched
    __try
    {
        if (*reinterpret_cast<WORD*>(csMonHdr) != 0xC005)
            return r;  // CS_MONITOR not emitted (single monitor / UseMultimon=0) -- CS_CORE nudge covers it
        DWORD count = *reinterpret_cast<DWORD*>(static_cast<BYTE*>(csMonHdr) + 8);
        if (count < 1 || count > 16)
        {
            Log(L"rdpedisp: nudge skipped -- implausible monitor count=%lu", count);
            return r;
        }
        for (DWORD i = 0; i < count; ++i)
        {
            BYTE* e = static_cast<BYTE*>(defArray) + i * 20;  // TS_MONITOR_DEF stride
            if (*reinterpret_cast<DWORD*>(e + 16) & 1u)       // primary (flags bit 0)
            {
                LONG  top    = *reinterpret_cast<LONG*>(e + 4);
                LONG* bottom = reinterpret_cast<LONG*>(e + 12);
                if (*bottom - top > static_cast<LONG>(g_nudgePx) + 64)  // keep a sane minimum height
                {
                    LONG before = *bottom;
                    *bottom -= static_cast<LONG>(g_nudgePx);
                    Log(L"rdpedisp: nudged connect-time CS_MONITOR primary height -%u px (bottom %ld->%ld) "
                        L"-- forcing host display reconfigure to (re)open display control",
                        g_nudgePx, static_cast<long>(before), static_cast<long>(*bottom));
                }
                return r;
            }
        }
        Log(L"rdpedisp: nudge skipped -- no primary monitor flagged among %lu", count);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(L"rdpedisp: CS_MONITOR-nudge FAULTED 0x%08lX -- connect untouched",
            static_cast<unsigned long>(GetExceptionCode()));
    }
    return r;
}

// CoreFSM::OnInitiateConnection(this, params, ...) copies desktopHeight from *(WORD*)(params+0x206)
// into the CS_CORE block. This is the size a single-monitor connect advertises. We transiently shave
// g_nudgePx off params+0x206 across the real call (so the whole CS_CORE build sees the smaller height),
// then restore it immediately -- connect-isolated, and the force-send later sends the true height.
int Hooked_OnInit(void* a1, void* params, void* a3, void* a4)
{
    if (params)  // capture the true CS_CORE desktop dims (used by the inject to build a consistent rect)
        __try {
            g_connectW = *reinterpret_cast<WORD*>(static_cast<BYTE*>(params) + 0x204);
            g_connectH = *reinterpret_cast<WORD*>(static_cast<BYTE*>(params) + 0x206);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

    WORD* ph = nullptr;
    WORD  saved = 0;
    bool  patched = false;
    if (g_nudgePx > 0 && g_fixEnabled && params)
    {
        __try
        {
            ph = reinterpret_cast<WORD*>(static_cast<BYTE*>(params) + 0x206);  // CS_CORE desktopHeight source
            WORD cur = *ph;
            if (cur > g_nudgePx + 64)  // sane floor -- never shrink toward a tiny/zero height
            {
                saved = cur;
                *ph = static_cast<WORD>(cur - g_nudgePx);
                patched = true;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { patched = false; ph = nullptr; }
    }

    int r = Real_OnInit(a1, params, a3, a4);  // builds CS_CORE from the (shaved) height

    if (patched)
    {
        __try
        {
            *ph = saved;  // restore at once -- only the GCC blob keeps the shaved height
            Log(L"rdpedisp: nudged connect-time CS_CORE desktopHeight -%u px (%u->%u) "
                L"-- forcing host display reconfigure to (re)open display control",
                g_nudgePx, static_cast<unsigned>(saved), static_cast<unsigned>(saved - g_nudgePx));
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
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
    // The connect-time nudge only *modifies* when DpiNudge>0 AND DpiFix is on (the nudge needs the
    // force-send to restore the resolution). In a diag build we install the hooks unconditionally so
    // they can OBSERVE the connect-time CS_MONITOR / CS_MONITOR_EX blocks (zero adjustment when off).
    // OnInitiateConnection (CS_CORE) is the primary nudge; NC_GetMONITORData (CS_MONITOR/EX) is multimon.
#ifdef MSTSFENCE_DPI_DIAG
    const bool armNudgeHooks = true;   // always hook to observe; modifications fire only per their own flags
#else
    const bool armNudgeHooks = (g_nudgePx > 0 && g_fixEnabled) || g_injectScale > 0;
#endif
    if (armNudgeHooks)
    {
        Real_OnInit = reinterpret_cast<OnInitConn_t>(base + kb->onInit);
        DetourAttach(&reinterpret_cast<PVOID&>(Real_OnInit), Hooked_OnInit);
        Real_GetMonData = reinterpret_cast<GetMonData_t>(base + kb->getMonData);
        DetourAttach(&reinterpret_cast<PVOID&>(Real_GetMonData), Hooked_GetMonData);
    }
    else if (g_nudgePx > 0)
    {
        Log(L"rdpedisp: DpiNudge=%u ignored -- needs DpiFix on (no force-send to restore the resolution)",
            g_nudgePx);
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
    Log(L"rdpedisp: channel hooks installed (build matched); commit=%ld fix=%d override=%d(%u%%) nudge=%upx inject=%u%%%s",
        err, g_fixEnabled, g_overrideEnabled, g_overridePct, g_nudgePx, g_injectScale,
        ((g_nudgePx || g_injectScale) && Real_OnInit) ? L" (armed)" : L"");
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
    if (Real_GetMonData)     DetourDetach(&reinterpret_cast<PVOID&>(Real_GetMonData), Hooked_GetMonData);
    if (Real_OnInit)         DetourDetach(&reinterpret_cast<PVOID&>(Real_OnInit), Hooked_OnInit);
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
    g_nudgePx = mstsfence::DpiNudgePx();
    g_injectScale = mstsfence::DpiInjectScale();

#ifdef MSTSFENCE_DPI_DIAG
    InstallApiProbes();   // observe-only DPI reads; always hook mstscax too (we want the data)
#else
    if (!g_fixEnabled && !g_overrideEnabled && g_nudgePx == 0 && g_injectScale == 0)
    {
        Log(L"rdpedisp: idle (DpiFix + DpiOverride + DpiNudge + DpiInjectScale all off).");
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
