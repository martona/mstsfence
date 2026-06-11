// rdpedisp.cpp -- see rdpedisp.h. The connect-time host-scale override plus
// (behind MSTSFENCE_DPI_DIAG) the observe-hooks and DPI API probes that
// reverse-engineered it. Self-contained: its own logger, its own mstscax
// resolution; hook.cpp just calls OnAttach()/OnDetach().

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
#include "mstscax_builds.h"

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
bool g_overrideEnabled = false;  // HKCU\Software\mstsfence\DpiOverride: rewrite the host scale to a chosen %
unsigned g_overridePct = 100;    // HKCU\Software\mstsfence\DpiOverridePct: 100..500
LONG g_connectW = 0;             // desktop dims captured from OnInitiateConnection (CS_CORE), for connect spoofing
LONG g_connectH = 0;
LONG g_layoutInstalled = 0;      // channel hooks installed (once mstscax is in)

using SendLayoutArr_t = int (*)(void* thisptr, unsigned int count, void* monitors); // Write / Send(array)
// CNC::NC_GetMONITORData(this, csMonitorHdr, monitorDefArray, +7 more) -- the connect-time CS_MONITOR
// builder. 10 args (rcx,rdx,r8,r9 + 6 on the stack); the full signature must match so the trampoline
// gets every arg. We only read arg2 (CS_MONITOR header) and arg3 (the DEF array).
using GetMonData_t    = int (*)(void*, void*, void*, void*, void*, void*, void*, void*, void*, void*);
// CoreFSM::OnInitiateConnection(this, params, +2 unused) -- builds the CS_CORE block. Uses only rcx/rdx
// (r8/r9 are clobbered before any read, no stack args), so a 4-arg pass-through trampoline is safe.
using OnInitConn_t    = int (*)(void*, void*, void*, void*);

SendLayoutArr_t Real_WriteLayout    = nullptr;  // the serializer every send funnels through (override point)
GetMonData_t    Real_GetMonData     = nullptr;  // connect-time CS_MONITOR/CS_MONITOR_EX builder
OnInitConn_t    Real_OnInit         = nullptr;  // connect-time CS_CORE builder
HMODULE(WINAPI* Real_LoadLibraryExW)(LPCWSTR, HANDLE, DWORD) = LoadLibraryExW;

// ---------------------------------------------------------------------------
// build identity -- the override only runs on an mstscax whose PDB GUID we recognise, so
// the hard-coded RVAs are guaranteed correct for that binary.
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
    DWORD write;       // WriteMonitorLayoutPdu(this,count,monitors) -- the override hooks this
    DWORD onInit;      // CoreFSM::OnInitiateConnection -- builds CS_CORE; we capture desktopWidth/desktopHeight
    DWORD getMonData;  // CNC::NC_GetMONITORData -- builds CS_MONITOR / CS_MONITOR_EX
    DWORD sendA;       // SendMonitorLayoutPdu(this,count,monitors)        ] diag
    DWORD validate;    // ValidateDisplayControlMonitorLayout(count,...)   ] observe
    DWORD gfx;         // RdpGfxClientChannel::SetMonitorLayout(this,...)   ] hooks
    DWORD gate;        // CMsTscAx::get_RemoteMonitorLayoutMatchesLocal     ] only
};
const KnownBuild kKnownBuilds[] = {
    // mstscax.dll PDB 6D429A36 (ships in 26100.8246 / .8328 / .8457 -- same binary).
    { mstsfence::rdpedisp::kMstscaxPdb_6D429A36,
      0x2AB2D4, 0x13EE24, 0x15F5B0, 0x2AB0A0, 0x2A8630, 0x289D18, 0x3965C0 },
};

// ---------------------------------------------------------------------------
// the layout struct + the optional host-scale override (always compiled)
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
typedef struct
{
    UINT32 physicalWidth;
    UINT32 physicalHeight;
    UINT32 orientation;
    UINT32 desktopScaleFactor;
    UINT32 deviceScaleFactor;
} MONITOR_ATTRIBUTES;

typedef struct
{
    INT32 x;
    INT32 y;
    INT32 width;
    INT32 height;
    UINT32 is_primary;
    UINT32 orig_screen;
    MONITOR_ATTRIBUTES attributes;
} rdpMonitor;

struct TS_MONITOR_DEF
{
    INT32 left;
    INT32 top;
    INT32 right;   // inclusive
    INT32 bottom;  // inclusive
    UINT32 flags;
};

struct CS_MONITOR_HEADER
{
    WORD type;
    WORD length;
    UINT32 flags;
    UINT32 monitorCount;
};

struct CS_MONITOR_EX_HEADER
{
    WORD type;
    WORD length;
    UINT32 flags;
    UINT32 monitorAttributeSize;
    UINT32 monitorCount;
};

struct DCML  // DISPLAYCONTROL_MONITOR_LAYOUT, 40 bytes; offsets confirmed from the validator disasm.
             // Flags: bit0 = DISPLAYCONTROL_MONITOR_PRIMARY (the only documented one; single monitor =>
             // always 1); bit1 = skip this monitor in the adjacency/overlap check (mstsc-internal, undoc).
{
    DWORD Flags; LONG Left; LONG Top; DWORD Width; DWORD Height;
    DWORD PhysicalWidth; DWORD PhysicalHeight; DWORD Orientation;
    DWORD DesktopScaleFactor; DWORD DeviceScaleFactor;
};
#pragma pack(pop)
static_assert(sizeof(MONITOR_ATTRIBUTES) == 20, "MONITOR_ATTRIBUTES must be 20 bytes");
static_assert(sizeof(rdpMonitor) == 44, "rdpMonitor must be 44 bytes");
static_assert(sizeof(TS_MONITOR_DEF) == 20, "TS_MONITOR_DEF must be 20 bytes");
static_assert(sizeof(CS_MONITOR_HEADER) == 12, "CS_MONITOR_HEADER must be 12 bytes");
static_assert(sizeof(CS_MONITOR_EX_HEADER) == 16, "CS_MONITOR_EX_HEADER must be 16 bytes");
static_assert(sizeof(DCML) == 40, "DISPLAYCONTROL_MONITOR_LAYOUT must be 40 bytes");

constexpr WORD kCsMonitor = 0xC005;
constexpr WORD kCsMonitorEx = 0xC008;
constexpr UINT32 kMonitorPrimary = 0x00000001;
constexpr UINT32 kMonitorAttributeSize = sizeof(MONITOR_ATTRIBUTES);

// DeviceScaleFactor must be 100, 140, or 180; snap the desktop scale to the nearest tier.
DWORD DeviceScaleTierFor(unsigned int pct)
{
    if (pct >= 160) return 180;
    if (pct >= 120) return 140;
    if (pct >= 100) return 100;
    return 0;
}

UINT32 PhysicalMmFor(INT32 pixels, UINT32 desktopScaleFactor)
{
    const UINT32 dpi = 96u * desktopScaleFactor / 100u;
    if (pixels <= 0 || dpi == 0)
        return 10;
    UINT32 mm = static_cast<UINT32>(static_cast<long long>(pixels) * 254 / (dpi * 10));
    if (mm < 10) return 10;
    if (mm > 10000) return 10000;
    return mm;
}

rdpMonitor MakePrimaryMonitor(INT32 width, INT32 height, UINT32 desktopScaleFactor)
{
    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;

    rdpMonitor mon{};
    mon.width = width;
    mon.height = height;
    mon.is_primary = 1;
    mon.attributes.physicalWidth = PhysicalMmFor(width, desktopScaleFactor);
    mon.attributes.physicalHeight = PhysicalMmFor(height, desktopScaleFactor);
    mon.attributes.orientation = 0;
    mon.attributes.desktopScaleFactor = desktopScaleFactor;
    mon.attributes.deviceScaleFactor = DeviceScaleTierFor(desktopScaleFactor);
    return mon;
}

void ApplyAttributesOverride(UINT32 count, MONITOR_ATTRIBUTES* attributes)
{
    if (!g_overrideEnabled || !attributes || count == 0 || count > 16)
        return;

    const UINT32 desktopScaleFactor = g_overridePct;
    const UINT32 deviceScaleFactor = DeviceScaleTierFor(desktopScaleFactor);
    for (UINT32 i = 0; i < count; ++i)
    {
        attributes[i].desktopScaleFactor = desktopScaleFactor;
        attributes[i].deviceScaleFactor = deviceScaleFactor;
    }
}

void WriteMonitorDef(const rdpMonitor& mon, TS_MONITOR_DEF* out)
{
    out->left = mon.x;
    out->top = mon.y;
    out->right = mon.x + mon.width - 1;
    out->bottom = mon.y + mon.height - 1;
    out->flags = mon.is_primary ? kMonitorPrimary : 0;
}

void WriteMonitorAttributes(const rdpMonitor& mon, MONITOR_ATTRIBUTES* out)
{
    *out = mon.attributes;
}

// Rewrite every monitor's scale to the configured % (clamped 100..500) before it ships.
void ApplyOverride(unsigned int count, void* dcmlArray)
{
    if (!g_overrideEnabled || !dcmlArray || count == 0 || count > 16)
        return;

    DWORD device = DeviceScaleTierFor(g_overridePct);
    DCML* m = reinterpret_cast<DCML*>(dcmlArray);
    for (unsigned i = 0; i < count; ++i)
    {
        m[i].DesktopScaleFactor = g_overridePct;
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
// connect-time monitor data override
// ---------------------------------------------------------------------------
// Single-monitor mstsc connections normally advertise only CS_CORE dimensions;
// they skip CS_MONITOR/CS_MONITOR_EX, so no desktop scale reaches the host at
// connect. When DpiOverride is on, synthesize the missing monitor blocks. When
// mstsc already emitted them, rewrite the scale fields in the existing
// TS_MONITOR_ATTRIBUTES array.
int Hooked_GetMonData(void* self, void* csMonitorHdr, void* monitorDefArray,
                      void* csMonitorLenOut, void* csMonitorExHdr,
                      void* monitorAttributes, void* csMonitorExLenOut,
                      void* monitorEx2Hdr, void* monitorEx2Data,
                      void* monitorEx2LenOut)
{
    int r = Real_GetMonData(self, csMonitorHdr, monitorDefArray, csMonitorLenOut,
                            csMonitorExHdr, monitorAttributes, csMonitorExLenOut,
                            monitorEx2Hdr, monitorEx2Data, monitorEx2LenOut);
#ifdef MSTSFENCE_DPI_DIAG
    __try
    {
        const auto* csmHdr = static_cast<const CS_MONITOR_HEADER*>(csMonitorHdr);
        const auto* cseHdr = static_cast<const CS_MONITOR_EX_HEADER*>(csMonitorExHdr);
        WORD csm = csmHdr ? csmHdr->type : 0;
        WORD cse = cseHdr ? cseHdr->type : 0;
        Log(L"GETMON: r=%d CS_MONITOR=0x%04X CS_MONITOR_EX=0x%04X%s", r, csm, cse,
            (csm != kCsMonitor && cse != kCsMonitorEx) ? L"  (nothing emitted -> no connect-time scale advertised)" : L"");
        if (cse == kCsMonitorEx && monitorAttributes)
        {
            const DWORD exCount = cseHdr->monitorCount;
            const auto* attrs = static_cast<const MONITOR_ATTRIBUTES*>(monitorAttributes);
            if (exCount >= 1 && exCount <= 16)
                for (DWORD i = 0; i < exCount; ++i)
                {
                    Log(L"GETMON:   MON_EX[%lu] physW=%lumm physH=%lumm orient=%lu DesktopScale=%lu%% DeviceScale=%lu%%",
                        static_cast<unsigned long>(i),
                        static_cast<unsigned long>(attrs[i].physicalWidth),
                        static_cast<unsigned long>(attrs[i].physicalHeight),
                        static_cast<unsigned long>(attrs[i].orientation),
                        static_cast<unsigned long>(attrs[i].desktopScaleFactor),
                        static_cast<unsigned long>(attrs[i].deviceScaleFactor));
                }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log(L"GETMON: observe faulted"); }
#endif
    if (!g_overrideEnabled || r < 0 || !csMonitorHdr || !monitorDefArray ||
        !csMonitorLenOut || !csMonitorExHdr || !monitorAttributes || !csMonitorExLenOut)
        return r;

    auto* csmHdr = static_cast<CS_MONITOR_HEADER*>(csMonitorHdr);
    auto* defs = static_cast<TS_MONITOR_DEF*>(monitorDefArray);
    auto* cseHdr = static_cast<CS_MONITOR_EX_HEADER*>(csMonitorExHdr);
    auto* attrs = static_cast<MONITOR_ATTRIBUTES*>(monitorAttributes);
    auto* csmLen = static_cast<DWORD*>(csMonitorLenOut);
    auto* cseLen = static_cast<DWORD*>(csMonitorExLenOut);

    __try
    {
        if (csmHdr->type == kCsMonitor)
        {
            const UINT32 count = csmHdr->monitorCount;
            if (count < 1 || count > 16)
            {
                Log(L"rdpedisp: connect override skipped -- implausible monitor count=%lu",
                    static_cast<unsigned long>(count));
                return r;
            }

            if (cseHdr->type != kCsMonitorEx)
            {
                for (UINT32 i = 0; i < count; ++i)
                {
                    const INT32 width = defs[i].right - defs[i].left + 1;
                    const INT32 height = defs[i].bottom - defs[i].top + 1;
                    attrs[i].physicalWidth = PhysicalMmFor(width, g_overridePct);
                    attrs[i].physicalHeight = PhysicalMmFor(height, g_overridePct);
                    attrs[i].orientation = 0;
                }
            }
            ApplyAttributesOverride(count, attrs);

            cseHdr->type = kCsMonitorEx;
            cseHdr->length = static_cast<WORD>(sizeof(CS_MONITOR_EX_HEADER) + count * sizeof(MONITOR_ATTRIBUTES));
            cseHdr->flags = 0;
            cseHdr->monitorAttributeSize = kMonitorAttributeSize;
            cseHdr->monitorCount = count;
            *cseLen = cseHdr->length;

            Log(L"rdpedisp: spoofed connect-time CS_MONITOR_EX scale to %u%% for %lu monitor(s)",
                g_overridePct, static_cast<unsigned long>(count));
            return r;
        }

        const rdpMonitor mon = MakePrimaryMonitor(g_connectW, g_connectH, g_overridePct);
        csmHdr->type = kCsMonitor;
        csmHdr->length = static_cast<WORD>(sizeof(CS_MONITOR_HEADER) + sizeof(TS_MONITOR_DEF));
        csmHdr->flags = 0;
        csmHdr->monitorCount = 1;
        WriteMonitorDef(mon, defs);

        cseHdr->type = kCsMonitorEx;
        cseHdr->length = static_cast<WORD>(sizeof(CS_MONITOR_EX_HEADER) + sizeof(MONITOR_ATTRIBUTES));
        cseHdr->flags = 0;
        cseHdr->monitorAttributeSize = kMonitorAttributeSize;
        cseHdr->monitorCount = 1;
        WriteMonitorAttributes(mon, attrs);

        *csmLen = csmHdr->length;
        *cseLen = cseHdr->length;
        Log(L"rdpedisp: injected CS_MONITOR+EX (1 mon %ldx%ld phys=%lux%lumm DesktopScale=%lu%% DeviceScale=%lu%%) at connect",
            static_cast<long>(mon.width), static_cast<long>(mon.height),
            static_cast<unsigned long>(mon.attributes.physicalWidth),
            static_cast<unsigned long>(mon.attributes.physicalHeight),
            static_cast<unsigned long>(mon.attributes.desktopScaleFactor),
            static_cast<unsigned long>(mon.attributes.deviceScaleFactor));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(L"rdpedisp: connect-time scale spoof FAULTED 0x%08lX -- connect left as-is",
            static_cast<unsigned long>(GetExceptionCode()));
    }
    return r;
}

// CoreFSM::OnInitiateConnection(this, params, ...) builds CS_CORE from
// *(WORD*)(params+0x204/0x206). Capture those dimensions so the single-monitor
// CS_MONITOR spoof matches the CS_CORE size mstsc is already advertising.
int Hooked_OnInit(void* a1, void* params, void* a3, void* a4)
{
    if (params)  // capture the true CS_CORE desktop dims (used by the spoof to build a consistent rect)
        __try {
            g_connectW = *reinterpret_cast<WORD*>(static_cast<BYTE*>(params) + 0x204);
            g_connectH = *reinterpret_cast<WORD*>(static_cast<BYTE*>(params) + 0x206);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}

    return Real_OnInit(a1, params, a3, a4);
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
            L"guid={%08lX-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X} -- DPI override not applied; send me this line",
            g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        return true;  // safe no-op
    }

    BYTE* base = reinterpret_cast<BYTE*>(mod);

    // The serializer is only hooked when the override is on (or in a diag build to observe).
#ifdef MSTSFENCE_DPI_DIAG
    bool wantWrite = true;
#else
    bool wantWrite = g_overrideEnabled;
#endif

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (wantWrite)
    {
        Real_WriteLayout = reinterpret_cast<SendLayoutArr_t>(base + kb->write);
        DetourAttach(&reinterpret_cast<PVOID&>(Real_WriteLayout), Hooked_WriteLayout);
    }
    // In a diag build, install the connect-time hooks unconditionally so they can observe
    // CS_MONITOR / CS_MONITOR_EX even when the override is off.
#ifdef MSTSFENCE_DPI_DIAG
    const bool wantConnect = true;
#else
    const bool wantConnect = g_overrideEnabled;
#endif
    if (wantConnect)
    {
        Real_OnInit = reinterpret_cast<OnInitConn_t>(base + kb->onInit);
        DetourAttach(&reinterpret_cast<PVOID&>(Real_OnInit), Hooked_OnInit);
        Real_GetMonData = reinterpret_cast<GetMonData_t>(base + kb->getMonData);
        DetourAttach(&reinterpret_cast<PVOID&>(Real_GetMonData), Hooked_GetMonData);
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
#endif
    LONG err = DetourTransactionCommit();
    Log(L"rdpedisp: channel hooks installed (build matched); commit=%ld override=%d(%u%%)%s",
        err, g_overrideEnabled, g_overridePct, Real_OnInit ? L" (connect armed)" : L"");
    return true;
}

void RemoveChannelHooks()
{
    if (!g_layoutInstalled)
        return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (Real_WriteLayout)    DetourDetach(&reinterpret_cast<PVOID&>(Real_WriteLayout), Hooked_WriteLayout);
    if (Real_GetMonData)     DetourDetach(&reinterpret_cast<PVOID&>(Real_GetMonData), Hooked_GetMonData);
    if (Real_OnInit)         DetourDetach(&reinterpret_cast<PVOID&>(Real_OnInit), Hooked_OnInit);
#ifdef MSTSFENCE_DPI_DIAG
    if (Real_SendLayoutA)  DetourDetach(&reinterpret_cast<PVOID&>(Real_SendLayoutA), Hooked_SendLayoutA);
    if (Real_Validate)     DetourDetach(&reinterpret_cast<PVOID&>(Real_Validate), Hooked_Validate);
    if (Real_GfxSetLayout) DetourDetach(&reinterpret_cast<PVOID&>(Real_GfxSetLayout), Hooked_GfxSetLayout);
    if (Real_MatchesLocal) DetourDetach(&reinterpret_cast<PVOID&>(Real_MatchesLocal), Hooked_MatchesLocal);
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
    g_overrideEnabled = mstsfence::DpiOverrideEnabled();
    g_overridePct = mstsfence::DpiOverridePct();

#ifdef MSTSFENCE_DPI_DIAG
    InstallApiProbes();   // observe-only DPI reads; always hook mstscax too (we want the data)
#else
    if (!g_overrideEnabled)
    {
        Log(L"rdpedisp: idle (DpiOverride off).");
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
