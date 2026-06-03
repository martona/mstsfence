// mstsfence -- the controller (tray app).
//
// Installs a global WH_CBT hook whose DLL (mstsfencehook.dll) the OS maps into GUI
// processes as they run. The DLL is inert except in mstsc.exe, where it clamps
// the screen-size APIs to the monitor work area so mstsc's full-screen window and
// session resolution leave the host taskbar visible, at native 1:1.
//
// A global hook (not launch-and-inject) on purpose: we don't control how mstsc
// starts -- .rdp files, taskbar jump lists and the Start menu all launch it
// directly. The hook catches mstsc however it starts; the DLL filters by exe name.
//
// Runs from the tray:
//   * survives Explorer restarts (re-adds the icon on TaskbarCreated)
//   * registers itself to run at login when it starts
//   * the tray "Exit" item unregisters autostart; WM_CLOSE / logoff do not
//
//   mstsfence            run in the tray (default)
//   mstsfence --diag     dump each monitor's rcMonitor vs rcWork, then exit
//   mstsfence --help     usage
//
// Set MSTSFENCE_TRACE=1 in mstsc's environment to get the DLL's diagnostic log back
// (%TEMP%\mstsfencehook.log); it is silent otherwise.

#include <windows.h>

#include <commctrl.h>
#include <shellapi.h>

#include <umbra.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "version.h"
#include "resource.h"

// ---------------------------------------------------------------------------
// shared helpers
// ---------------------------------------------------------------------------
// Full path to mstsfencehook.dll next to this exe (independent of the CWD).
static bool HookDllPath(wchar_t* out, size_t cap)
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return false;
    }
    wchar_t* slash = wcsrchr(exe, L'\\');
    if (!slash)
    {
        return false;
    }
    *(slash + 1) = L'\0';  // keep trailing backslash
    return wcscpy_s(out, cap, exe) == 0 && wcscat_s(out, cap, L"mstsfencehook.dll") == 0;
}

// ---------------------------------------------------------------------------
// --diag : monitor rect dump (same data the hook manipulates)
// ---------------------------------------------------------------------------
static BOOL CALLBACK MonitorEnumProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam)
{
    int* index = reinterpret_cast<int*>(lParam);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi))
    {
        return TRUE;
    }

    const RECT& m = mi.rcMonitor;
    const RECT& w = mi.rcWork;
    const LONG monW = m.right - m.left, monH = m.bottom - m.top;
    const LONG workW = w.right - w.left, workH = w.bottom - w.top;

    wprintf(L"  [%d] %s%s\n", *index, mi.szDevice,
            (mi.dwFlags & MONITORINFOF_PRIMARY) ? L"  (primary)" : L"");
    wprintf(L"      rcMonitor : (%ld,%ld)-(%ld,%ld)  %ldx%ld\n",
            m.left, m.top, m.right, m.bottom, monW, monH);
    wprintf(L"      rcWork    : (%ld,%ld)-(%ld,%ld)  %ldx%ld\n",
            w.left, w.top, w.right, w.bottom, workW, workH);
    if (monW != workW || monH != workH)
    {
        wprintf(L"      taskbar reserves %ld px wide / %ld px tall\n", monW - workW, monH - workH);
    }
    else
    {
        wprintf(L"      work area == monitor (no taskbar reservation here)\n");
    }

    ++*index;
    return TRUE;
}

// A GUI-subsystem exe has no console; attach to the parent's (or allocate one)
// so --diag / --help can print.
static void EnsureConsole()
{
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
    {
        AllocConsole();
    }
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}

static int RunDiag()
{
    wprintf(L"Monitors as the system reports them:\n");
    int index = 0;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc, reinterpret_cast<LPARAM>(&index));
    if (index == 0)
    {
        wprintf(L"  (none enumerated)\n");
    }
    return 0;
}

static int RunHelp()
{
    wprintf(L"mstsfence v%hs\n", MSTSFENCE_VERSION_STRING);
    wprintf(L"Keep mstsc's full-screen window (and session resolution) within the\n"
            L"monitor work area so the host taskbar stays visible.\n\n");
    wprintf(L"Usage:\n");
    wprintf(L"  mstsfence            run in the tray; install the global hook; register at login\n");
    wprintf(L"  mstsfence --diag     print each monitor's rcMonitor vs rcWork and exit\n");
    wprintf(L"  mstsfence --help     this help\n\n");
    wprintf(L"Set MSTSFENCE_TRACE=1 (in mstsc's environment) for the hook's diagnostic log.\n");
    return 0;
}

// ---------------------------------------------------------------------------
// tray controller
// ---------------------------------------------------------------------------
#define WM_TRAY (WM_APP + 1)
enum
{
    ID_ABOUT = 1001,
    ID_EXIT = 1002,
};
static const UINT TRAY_UID = 1;

static const wchar_t* RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_VALUE = L"mstsfence";

static HHOOK g_hook = nullptr;
static HMODULE g_dll = nullptr;
static HWND g_hwnd = nullptr;
static UINT g_wmTaskbarCreated = 0;

static void RegisterAutostart()
{
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
    {
        return;
    }
    wchar_t cmd[MAX_PATH + 3];
    if (swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%s\"", exe) < 0)
    {
        return;
    }
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) == ERROR_SUCCESS)
    {
        RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd),
                       static_cast<DWORD>((wcslen(cmd) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
}

static void UnregisterAutostart()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        RegDeleteValueW(key, RUN_VALUE);
        RegCloseKey(key);
    }
}

static void AddTrayIcon(HWND hwnd)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                              MAKEINTRESOURCEW(IDI_MSTSFENCE_ICON), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXSMICON),
                                              GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (nid.hIcon == nullptr)
        nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);  // fallback if the resource is missing
    wcscpy_s(nid.szTip, ARRAYSIZE(nid.szTip), L"mstsfence");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon(HWND hwnd)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

#define GITHUB_URL L"https://github.com/martona/mstsfence"

// Attribution for Microsoft Detours (umbra is the author's own, so omitted).
static const wchar_t kDetoursNotice[] =
    L"This product uses Microsoft Detours — Copyright (c) Microsoft Corporation, "
    L"licensed under the MIT License.";

static INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        // Large icon (the 64px frame) for the dialog body; a small one for the title bar.
        HICON hBig = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_MSTSFENCE_ICON), IMAGE_ICON, 64, 64, LR_SHARED));
        if (hBig)
            SendDlgItemMessageW(hDlg, IDC_ABOUT_ICON, STM_SETICON, reinterpret_cast<WPARAM>(hBig), 0);
        HICON hSmall = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_MSTSFENCE_ICON), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        if (hSmall)
            SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hSmall));

        wchar_t ver[64];
        swprintf_s(ver, ARRAYSIZE(ver), L"Version %hs", MSTSFENCE_VERSION_STRING);
        SetDlgItemTextW(hDlg, IDC_ABOUT_NAME, L"mstsfence");
        SetDlgItemTextW(hDlg, IDC_ABOUT_VERSION, ver);
        SetDlgItemTextW(hDlg, IDC_ABOUT_COPYRIGHT, L"Copyright © 2026 Marton Anka");
        SetDlgItemTextW(hDlg, IDC_ABOUT_LINK,
            L"<a href=\"" GITHUB_URL L"\">github.com/martona/mstsfence</a>");
        SetDlgItemTextW(hDlg, IDC_ABOUT_DETOURS, kDetoursNotice);

        // Dark-style the dialog and its controls via umbra (initialized at startup).
        umbra::setDarkTitleBarEx(hDlg, true);
        umbra::setWindowEraseBgSubclass(hDlg);
        umbra::setDarkWndNotifySafe(hDlg, true);
        umbra::setDarkThemeExperimental(GetDlgItem(hDlg, IDOK), L"Explorer");
        return TRUE;
    }

    case WM_NOTIFY:
    {
        auto nmhdr = reinterpret_cast<LPNMHDR>(lParam);
        if (nmhdr->idFrom == IDC_ABOUT_LINK && (nmhdr->code == NM_CLICK || nmhdr->code == NM_RETURN))
        {
            ShellExecuteW(hDlg, L"open", GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
        {
            EndDialog(hDlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void ShowAbout(HWND owner)
{
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT), owner, AboutDlgProc, 0);
}

static void ShowContextMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        return;
    }
    AppendMenuW(menu, MF_STRING, ID_ABOUT, L"About…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");

    SetForegroundWindow(hwnd);  // so the menu dismisses on click-away
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);  // classic TrackPopupMenu dismissal fix
}

static bool InstallHook()
{
    wchar_t dllPath[MAX_PATH];
    if (!HookDllPath(dllPath, MAX_PATH))
    {
        return false;
    }
    g_dll = LoadLibraryW(dllPath);
    if (!g_dll)
    {
        return false;
    }
    HOOKPROC proc = reinterpret_cast<HOOKPROC>(GetProcAddress(g_dll, "CBTProc"));
    if (!proc)
    {
        return false;
    }
    g_hook = SetWindowsHookExW(WH_CBT, proc, g_dll, 0);  // 0 == all threads (global)
    return g_hook != nullptr;
}

static void RemoveHook()
{
    if (g_hook)
    {
        UnhookWindowsHookEx(g_hook);
        g_hook = nullptr;
    }
    if (g_dll)
    {
        FreeLibrary(g_dll);
        g_dll = nullptr;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == g_wmTaskbarCreated)  // Explorer restarted -> re-add the icon
    {
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (msg)
    {
    case WM_TRAY:
        switch (LOWORD(lParam))
        {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            ShowContextMenu(hwnd);
            break;
        case WM_LBUTTONDBLCLK:
            ShowAbout(hwnd);
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_ABOUT:
            ShowAbout(hwnd);
            break;
        case ID_EXIT:
            UnregisterAutostart();  // manual exit -> stop running at login
            DestroyWindow(hwnd);
            break;
        }
        return 0;

    case WM_DESTROY:
        // Reached by every teardown path (Exit, WM_CLOSE, logoff). Clean up the
        // icon + hook here, but DON'T touch autostart -- only the Exit handler
        // above unregisters, so a logoff/Explorer-kill leaves us armed for login.
        RemoveTrayIcon(hwnd);
        RemoveHook();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static int RunTray(HINSTANCE hinst)
{
    // Single instance: a second launch (e.g. manual run while the login copy is
    // up) bows out so we don't stack two tray icons / two hooks.
    HANDLE mtx = CreateMutexW(nullptr, FALSE, L"Local\\mstsfence.controller");
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(mtx);
        return 0;
    }

    // Common controls v6 (SysLink + subclassing in the About dialog) and dark
    // mode (dark tray popup menu + a themable About dialog) -- both process-wide.
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);
    umbra::initDarkMode();

    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = L"MSTSFENCE_Controller";
    if (!RegisterClassExW(&wc))
    {
        return 1;
    }

    // A real (top-level) window, never shown -- message-only windows don't
    // receive the TaskbarCreated broadcast, so we need this kind.
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"mstsfence", WS_OVERLAPPED, 0, 0, 0,
                             0, nullptr, nullptr, hinst, nullptr);
    if (!g_hwnd)
    {
        return 1;
    }

    RegisterAutostart();  // running => registered to run at login

    if (!InstallHook())
    {
        MessageBoxW(g_hwnd,
                    L"Could not install the WH_CBT hook -- mstsc windows won't be adjusted.\n"
                    L"(Is mstsfencehook.dll next to mstsfence.exe?)",
                    L"mstsfence", MB_OK | MB_ICONWARNING);
        // keep running so the tray is still usable / Exit can unregister
    }

    AddTrayIcon(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int)
{
    int mode = 0;  // 0=tray, 1=diag, 2=help, -1=bad arg
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (_wcsicmp(argv[i], L"--diag") == 0)
            {
                mode = 1;
            }
            else if (_wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"-h") == 0 ||
                     _wcsicmp(argv[i], L"/?") == 0)
            {
                mode = 2;
            }
            else
            {
                mode = -1;
            }
            break;
        }
        LocalFree(argv);
    }

    switch (mode)
    {
    case 1:
        EnsureConsole();
        return RunDiag();
    case 2:
        EnsureConsole();
        return RunHelp();
    case -1:
        EnsureConsole();
        fwprintf(stderr, L"unknown argument (try --help)\n");
        return 2;
    default:
        return RunTray(hinst);
    }
}
