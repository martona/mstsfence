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
#include <commctrl.h>

#include "version.h"
#include "resource.h"
#include "settings.h"

#pragma comment(lib, "Comctl32.lib")

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
// tray controller
// ---------------------------------------------------------------------------
#define WM_TRAY (WM_APP + 1)
enum
{
    ID_ABOUT = 1001,
    ID_EXIT = 1002,
    ID_SETTINGS = 1003,
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

// Common dark-styling for our own modal dialogs (About / Settings).
static void DarkenDialog(HWND hDlg)
{
    umbra::setDarkTitleBarEx(hDlg, true);
    umbra::setWindowEraseBgSubclass(hDlg);
    umbra::setDarkWndNotifySafe(hDlg, true);
}

// Settings dialog. Reads/writes HKCU\Software\mstsfence; the hook picks the values up when a
// new mstsc session starts. 
static INT_PTR CALLBACK SettingsDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        SetForegroundWindow(hDlg);
        SetActiveWindow(hDlg);
        CheckDlgButton(hDlg, IDC_SET_FENCE, mstsfence::FenceEnabled()    ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_SET_DARK,  mstsfence::DarkModeEnabled() ? BST_CHECKED : BST_UNCHECKED);

        // DPI override controls.
        CheckDlgButton(hDlg, IDC_SET_DPIOVER, mstsfence::DpiOverrideEnabled() ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hDlg, IDC_SET_DPIPCT, mstsfence::DpiOverridePct(), FALSE);
        SendDlgItemMessageW(hDlg, IDC_SET_DPIPCT, EM_SETLIMITTEXT, 3, 0);
        EnableWindow(GetDlgItem(hDlg, IDC_SET_DPIPCT), mstsfence::DpiOverrideEnabled());

        DarkenDialog(hDlg);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_SET_DPIOVER:  // the % edit is only usable when the override is on
            {
                EnableWindow(GetDlgItem(hDlg, IDC_SET_DPIPCT),
                            IsDlgButtonChecked(hDlg, IDC_SET_DPIOVER) == BST_CHECKED);
                return TRUE;
            }
        case IDOK:
            {
                mstsfence::SetFenceEnabled(IsDlgButtonChecked(hDlg, IDC_SET_FENCE) == BST_CHECKED);
                mstsfence::SetDarkModeEnabled(IsDlgButtonChecked(hDlg, IDC_SET_DARK) == BST_CHECKED);
                mstsfence::SetDpiOverrideEnabled(IsDlgButtonChecked(hDlg, IDC_SET_DPIOVER) == BST_CHECKED);
                BOOL parsed = FALSE;
                UINT pct = GetDlgItemInt(hDlg, IDC_SET_DPIPCT, &parsed, FALSE);
                if (parsed)
                    mstsfence::SetDpiOverridePct(pct);  // clamps to 100..500
                EndDialog(hDlg, IDOK);
                return TRUE;
            }
        case IDCANCEL:
            {
                EndDialog(hDlg, IDCANCEL);
                return TRUE;
            }
        }
        break;
    }
    return FALSE;
}

static void ShowSettings(HWND owner)
{
    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS), owner, SettingsDlgProc, 0);
}

static INT_PTR CALLBACK AboutDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static HICON hIcon = nullptr;

    switch (msg)
    {
        case WM_INITDIALOG:
        {
            SetForegroundWindow(hDlg);
            SetActiveWindow(hDlg);
            LoadIconWithScaleDown(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_MSTSFENCE_ICON), 256, 256, &hIcon);

            wchar_t ver[64];
            swprintf_s(ver, ARRAYSIZE(ver), L"Version %hs", MSTSFENCE_VERSION_STRING);
            SetDlgItemTextW(hDlg, IDC_ABOUT_NAME, L"mstsfence");
            SetDlgItemTextW(hDlg, IDC_ABOUT_VERSION, ver);
            SetDlgItemTextW(hDlg, IDC_ABOUT_COPYRIGHT, L"Copyright © 2026 Marton Anka");
            SetDlgItemTextW(hDlg, IDC_ABOUT_LINK,
                L"<a href=\"" GITHUB_URL L"\">github.com/martona/mstsfence</a>");
            SetDlgItemTextW(hDlg, IDC_ABOUT_DETOURS, kDetoursNotice);

            // Dark-style the dialog and its controls via umbra (initialized at startup).
            DarkenDialog(hDlg);
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

        case WM_DRAWITEM:
        {
            auto pDraw = reinterpret_cast<LPDRAWITEMSTRUCT>(lParam);
            if (pDraw->CtlID == IDC_ABOUT_ICON) 
            {
                int ctrlWidth = pDraw->rcItem.right - pDraw->rcItem.left;
                int ctrlHeight = pDraw->rcItem.bottom - pDraw->rcItem.top;
                int minDim = ctrlWidth < ctrlHeight ? ctrlWidth : ctrlHeight;
                DrawIconEx(
                    pDraw->hDC, 
                    0, 0,
                    hIcon,
                    minDim, minDim,
                    0,
                    nullptr, 
                    DI_NORMAL);
                return (INT_PTR)TRUE;
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
        
        case WM_DESTROY:
        {
            DestroyIcon(hIcon);
            return TRUE;
        }
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
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"Settings…");
    AppendMenuW(menu, MF_STRING, ID_ABOUT, L"About…");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_EXIT, L"Exit");
    SetMenuDefaultItem(menu, ID_SETTINGS, FALSE);
    
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
            ShowSettings(hwnd);
            break;
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case ID_SETTINGS:
            ShowSettings(hwnd);
            break;
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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    return RunTray(hinst);
}
