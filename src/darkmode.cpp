// Dark-styling of mstsc's UI via umbra (WM_UMBRA), driven by the controller's
// global WH_CBT hook. Adapted from the WM_NIGHT hook prototype, trimmed to what
// mstsc actually shows: top-level frame title bars, #32770 dialogs, and the common
// controls (combo / edit / button / static / listbox / scrollbar). The regedit-,
// ACLUI-, ListView- and TreeView-specific machinery from WM_NIGHT is dropped.
//
// CBT-only: HCBT_CREATEWND themes a frame, HCBT_ACTIVATE themes the frame + dialog
// + controls (dialogs enumerate their children). Late-built dialog children (the
// thing WM_NIGHT's extra WH_CALLWNDPROC hook caught) are not themed -- acceptable
// for mstsc's UI.
//
// Cleanup is passive: every subclass removes itself on WM_NCDESTROY, and the DLL
// pins itself once it subclasses anything, so a window keeps its theming until it
// closes and never holds a dangling subclass pointer.

#include "darkmode.h"
#include "settings.h"

#include <windows.h>
#include <commctrl.h>

#include <umbra.h>

namespace
{
    // ---- per-window state props + subclass ids ('WL' = 0x574C) -------------
    constexpr wchar_t kReadyProp[]   = L"MstsFenceDark.Ready";
    constexpr wchar_t kBackoffProp[] = L"MstsFenceDark.Backoff";

    constexpr UINT_PTR kDialogBackfillSubclassId   = 0x574C0001;
    constexpr UINT_PTR kComboBoxCtlColorSubclassId = 0x574C0002;

    INIT_ONCE     g_darkModeInitOnce  = INIT_ONCE_STATIC_INIT;
    volatile LONG g_hookModulePinned  = 0;
    bool          g_darkEnabled       = false;  // set in DarkModeOnAttach (off if MSTSFENCE_NODARK)

    // ====================================================================
    // module pin + one-time umbra init
    // ====================================================================
    bool PinHookModule() noexcept
    {
        if (::InterlockedCompareExchange(&g_hookModulePinned, 1, 0) != 0)
            return true;
        HMODULE module = nullptr;
        const BOOL pinned = ::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&PinHookModule), &module);
        if (!pinned)
        {
            ::InterlockedExchange(&g_hookModulePinned, 0);
            return false;
        }
        return true;
    }

    // Our own module handle (for the thread-local message hook below). Magic
    // static -> initialized once, thread-safe.
    HMODULE SelfModule() noexcept
    {
        static HMODULE mod = []() noexcept {
            HMODULE m = nullptr;
            ::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&SelfModule), &m);
            return m;
        }();
        return mod;
    }

    BOOL CALLBACK InitDarkModeOnce(PINIT_ONCE, PVOID, PVOID*) noexcept
    {
        umbra::initDarkMode();
        return TRUE;
    }

    bool EnsureDarkModeReady() noexcept
    {
        return ::InitOnceExecuteOnce(&g_darkModeInitOnce, InitDarkModeOnce, nullptr, nullptr) != FALSE;
    }

    // ====================================================================
    // window classification
    // ====================================================================
    bool SameTextIgnoreCase(const wchar_t* a, const wchar_t* b) noexcept
    {
        return a && b && ::CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
    }

    bool ClassIs(HWND hwnd, const wchar_t* expected) noexcept
    {
        if (hwnd == nullptr)
            return false;
        wchar_t name[128]{};
        if (::GetClassNameW(hwnd, name, ARRAYSIZE(name)) == 0)
            return false;
        return SameTextIgnoreCase(name, expected);
    }

    bool IsDialogWindow(HWND h) noexcept            { return ClassIs(h, L"#32770"); }
    bool IsComboBoxWindow(HWND h) noexcept          { return ClassIs(h, WC_COMBOBOX); }
    bool IsComboDropDownListWindow(HWND h) noexcept { return ClassIs(h, L"ComboLBox"); }
    bool IsListBoxWindow(HWND h) noexcept           { return ClassIs(h, WC_LISTBOX) || IsComboDropDownListWindow(h); }
    bool IsEditWindow(HWND h) noexcept              { return ClassIs(h, WC_EDIT); }
    bool IsScrollBarWindow(HWND h) noexcept         { return ClassIs(h, WC_SCROLLBAR); }
    bool IsButtonWindow(HWND h) noexcept            { return ClassIs(h, WC_BUTTON); }
    bool IsStaticWindow(HWND h) noexcept            { return ClassIs(h, WC_STATIC); }

    bool IsTopLevelCaptioned(HWND hwnd) noexcept
    {
        const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
        return (style & WS_CHILD) == 0 && (style & WS_CAPTION) != 0;
    }

    // ---- back-off zones: shell / DirectUI / legacy comdlg32 dialogs ---------
    // These are drawn by the OS/shell (and already dark); theming the standard
    // controls embedded in them breaks them or looks half-and-half. Leave native.
    bool IsComdlg32Dialog(HWND hwnd) noexcept
    {
        if (!IsDialogWindow(hwnd))
            return false;
        const LONG_PTR dlgProc = ::GetWindowLongPtrW(hwnd, DWLP_DLGPROC);
        if (dlgProc == 0)
            return false;
        HMODULE owner = nullptr;
        if (::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(dlgProc), &owner) == FALSE)
            return false;
        return owner == ::GetModuleHandleW(L"comdlg32.dll");
    }

    BOOL CALLBACK FindBackoffViewProc(HWND child, LPARAM lp) noexcept
    {
        if (ClassIs(child, L"DirectUIHWND") || ClassIs(child, L"DUIViewWndClassName") ||
            ClassIs(child, L"SHELLDLL_DefView"))
        {
            *reinterpret_cast<bool*>(lp) = true;
            return FALSE;
        }
        return TRUE;
    }

    bool ContainsBackoffView(HWND hwnd) noexcept
    {
        bool found = false;
        ::EnumChildWindows(hwnd, FindBackoffViewProc, reinterpret_cast<LPARAM>(&found));
        return found;
    }

    bool IsInUntouchableRoot(HWND hwnd) noexcept
    {
        const HWND root = ::GetAncestor(hwnd, GA_ROOT);
        if (root == nullptr)
            return false;
        if (::GetPropW(root, kBackoffProp) != nullptr)
            return true;
        if (IsComdlg32Dialog(root) || ContainsBackoffView(root))
        {
            ::SetPropW(root, kBackoffProp, reinterpret_cast<HANDLE>(1));
            return true;
        }
        return false;
    }

    bool MarkReady(HWND hwnd) noexcept
    {
        if (::GetPropW(hwnd, kReadyProp) != nullptr)
            return false;
        return ::SetPropW(hwnd, kReadyProp, reinterpret_cast<HANDLE>(1)) != FALSE;
    }

    // forward decls for the control darkener (used by the dialog backfill subclass)
    void ApplyControl(HWND hwnd) noexcept;
    void RefreshControls(HWND hwndRoot) noexcept;

    // ====================================================================
    // dialog background backfill (fills the gaps a themed dialog leaves light)
    // ====================================================================
    struct ExcludeChildrenCtx { HWND parent; HDC hdc; };

    BOOL CALLBACK ExcludeVisibleChildFromClip(HWND child, LPARAM param) noexcept
    {
        auto* ctx = reinterpret_cast<ExcludeChildrenCtx*>(param);
        if (!::IsWindowVisible(child))
            return TRUE;
        RECT rc{};
        if (!::GetWindowRect(child, &rc))
            return TRUE;
        POINT pts[2] = { { rc.left, rc.top }, { rc.right, rc.bottom } };
        ::MapWindowPoints(nullptr, ctx->parent, pts, 2);
        ::ExcludeClipRect(ctx->hdc, pts[0].x, pts[0].y, pts[1].x, pts[1].y);
        return TRUE;
    }

    void FillDialogClientBackground(HWND hwnd, HDC hdc, bool excludeChildren) noexcept
    {
        if (hwnd == nullptr || hdc == nullptr)
            return;
        RECT rcClient{};
        ::GetClientRect(hwnd, &rcClient);
        const int saved = ::SaveDC(hdc);
        if (excludeChildren && saved != 0)
        {
            ExcludeChildrenCtx ctx{ hwnd, hdc };
            ::EnumChildWindows(hwnd, ExcludeVisibleChildFromClip, reinterpret_cast<LPARAM>(&ctx));
        }
        ::FillRect(hdc, &rcClient, umbra::getDlgBackgroundBrush());
        if (saved != 0)
            ::RestoreDC(hdc, saved);
    }

    LRESULT CALLBACK DialogBackfillSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                            UINT_PTR subclassId, DWORD_PTR) noexcept
    {
        switch (msg)
        {
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, DialogBackfillSubclass, subclassId);
            ::RemovePropW(hwnd, kReadyProp);
            break;
        case WM_ERASEBKGND:
            if (umbra::isEnabled())
            {
                FillDialogClientBackground(hwnd, reinterpret_cast<HDC>(wp), false);
                return TRUE;
            }
            break;
        case WM_PAINT:
            if (umbra::isEnabled())
            {
                const LRESULT result = ::DefSubclassProc(hwnd, msg, wp, lp);
                HDC hdc = ::GetDC(hwnd);
                if (hdc != nullptr)
                {
                    FillDialogClientBackground(hwnd, hdc, true);
                    ::ReleaseDC(hwnd, hdc);
                }
                return result;
            }
            break;
        case WM_PARENTNOTIFY:
            if (LOWORD(wp) == WM_CREATE && lp != 0)
            {
                ApplyControl(reinterpret_cast<HWND>(lp));
                RefreshControls(hwnd);
            }
            break;
        case WM_SHOWWINDOW:
            if (wp != FALSE)
                RefreshControls(hwnd);
            break;
        default:
            break;
        }
        return ::DefSubclassProc(hwnd, msg, wp, lp);
    }

    void InstallDialogBackfillSubclass(HWND hwnd) noexcept
    {
        ::SetWindowSubclass(hwnd, DialogBackfillSubclass, kDialogBackfillSubclassId, 0);
    }

    // ====================================================================
    // combobox ctl-color
    // ====================================================================
    LRESULT CALLBACK ComboBoxCtlColorSubclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                              UINT_PTR subclassId, DWORD_PTR) noexcept
    {
        switch (msg)
        {
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(hwnd, ComboBoxCtlColorSubclass, subclassId);
            break;
        case WM_CTLCOLOREDIT:
            if (umbra::isEnabled())
                return umbra::onCtlColorCtrl(reinterpret_cast<HDC>(wp));
            break;
        case WM_CTLCOLORLISTBOX:
            if (umbra::isEnabled())
                return umbra::onCtlColorListbox(wp, lp);
            break;
        default:
            break;
        }
        return ::DefSubclassProc(hwnd, msg, wp, lp);
    }

    void SetComboBoxListTheme(HWND hwnd) noexcept
    {
        COMBOBOXINFO cbi{};
        cbi.cbSize = sizeof(cbi);
        if (::GetComboBoxInfo(hwnd, &cbi) == FALSE || cbi.hwndList == nullptr)
            return;
        umbra::setDarkExplorerTheme(cbi.hwndList);
        ::RedrawWindow(cbi.hwndList, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }

    // ====================================================================
    // per-control "apply dark mode" handlers
    // ====================================================================
    void InstallNotifySubclassesForControl(HWND hwnd) noexcept
    {
        HWND parent = ::GetParent(hwnd);
        for (int depth = 0; parent != nullptr && depth < 32; ++depth)
        {
            umbra::setWindowNotifyCustomDrawSubclass(parent);
            umbra::setWindowCtlColorSubclass(parent);
            if (IsDialogWindow(parent))
                break;
            parent = ::GetParent(parent);
        }
    }

    void ApplyComboBoxTheme(HWND hwnd) noexcept
    {
        umbra::setComboBoxCtrlSubclass(hwnd);
        ::SetWindowSubclass(hwnd, ComboBoxCtlColorSubclass, kComboBoxCtlColorSubclassId, 0);
        umbra::setDarkThemeExperimental(hwnd, L"CFD");
        SetComboBoxListTheme(hwnd);
    }

    void ApplyComboDropDownTheme(HWND hwnd) noexcept
    {
        umbra::setDarkExplorerTheme(hwnd);
        ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }

    void ApplyListBoxTheme(HWND hwnd) noexcept
    {
        umbra::setDarkExplorerTheme(hwnd);
        umbra::setCustomBorderForListBoxOrEditCtrlSubclass(hwnd);
        ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }

    void ApplyEditTheme(HWND hwnd) noexcept
    {
        // The dark look needs AllowDarkModeForWindow first (setDarkThemeExperimental
        // makes that call; setDarkExplorerTheme does not), then the dark "CFD" theme,
        // or a custom dark border for a legacy client-edge edit.
        InstallNotifySubclassesForControl(hwnd);
        umbra::setDarkThemeExperimental(hwnd, L"CFD");
        if ((::GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) == WS_EX_CLIENTEDGE)
        {
            umbra::setCustomBorderForListBoxOrEditCtrlSubclass(hwnd);
            umbra::setWindowExStyle(hwnd, !umbra::isEnabled(), WS_EX_CLIENTEDGE);
        }
        ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }

    void ApplyButtonTheme(HWND hwnd) noexcept
    {
        switch (::GetWindowLongPtrW(hwnd, GWL_STYLE) & BS_TYPEMASK)
        {
        case BS_CHECKBOX:    case BS_AUTOCHECKBOX:
        case BS_3STATE:      case BS_AUTO3STATE:
        case BS_RADIOBUTTON: case BS_AUTORADIOBUTTON:
            umbra::setDarkThemeExperimental(hwnd, L"Explorer");
            umbra::setCheckboxOrRadioBtnCtrlSubclass(hwnd);
            break;
        case BS_GROUPBOX:
            umbra::setGroupboxCtrlSubclass(hwnd);
            break;
        default:   // push / default-push / split
            umbra::setDarkThemeExperimental(hwnd, L"Explorer");
            break;
        }
        ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }

    void ApplyStaticTheme(HWND hwnd) noexcept
    {
        umbra::setStaticTextCtrlSubclass(hwnd);
        ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
    }

    void ApplyScrollBarTheme(HWND hwnd) noexcept
    {
        umbra::setDarkScrollBar(hwnd);
        ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }

    // ====================================================================
    // control darkener: dispatch by window class
    // ====================================================================
    struct ControlHandler
    {
        bool (*matches)(HWND) noexcept;
        void (*apply)(HWND) noexcept;
    };

    const ControlHandler* FindControlHandler(HWND hwnd) noexcept
    {
        static constexpr ControlHandler handlers[] =
        {
            { IsComboBoxWindow,          ApplyComboBoxTheme },
            { IsComboDropDownListWindow, ApplyComboDropDownTheme },
            { IsListBoxWindow,           ApplyListBoxTheme },
            { IsEditWindow,              ApplyEditTheme },
            { IsButtonWindow,            ApplyButtonTheme },
            { IsStaticWindow,            ApplyStaticTheme },
            { IsScrollBarWindow,         ApplyScrollBarTheme },
        };
        for (const auto& handler : handlers)
            if (handler.matches(hwnd))
                return &handler;
        return nullptr;
    }

    void ApplyControl(HWND hwnd) noexcept
    {
        if (!g_darkEnabled || hwnd == nullptr)
            return;
        if (IsInUntouchableRoot(hwnd))   // shell / DUI / comdlg32-owned: leave native
            return;
        const ControlHandler* handler = FindControlHandler(hwnd);
        if (handler == nullptr)
            return;
        if (!PinHookModule() || !EnsureDarkModeReady())
            return;
        handler->apply(hwnd);
    }

    BOOL CALLBACK RefreshControlProc(HWND hwnd, LPARAM) noexcept
    {
        ApplyControl(hwnd);
        return TRUE;
    }

    void RefreshControls(HWND hwndRoot) noexcept
    {
        if (!g_darkEnabled || hwndRoot == nullptr)
            return;
        ApplyControl(hwndRoot);
        ::EnumChildWindows(hwndRoot, RefreshControlProc, 0);
    }

    // ====================================================================
    // top-level apply: frames and #32770 dialogs
    // ====================================================================
    void ApplyDarkModeToDialog(HWND hwnd) noexcept
    {
        if (!g_darkEnabled || !IsDialogWindow(hwnd))
            return;
        if (IsInUntouchableRoot(hwnd))   // shell / DUI / comdlg32-owned: leave native
            return;
        if (!MarkReady(hwnd))
            return;
        if (!PinHookModule() || !EnsureDarkModeReady())
            return;

        umbra::setDarkTitleBarEx(hwnd, true);
        umbra::enableThemeDialogTexture(hwnd, false);
        umbra::setWindowEraseBgSubclass(hwnd);
        umbra::setDarkWndNotifySafe(hwnd, true);
        InstallDialogBackfillSubclass(hwnd);
        RefreshControls(hwnd);
        ::RedrawWindow(hwnd, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW);
    }

    // Generic top-level frame (the mstsc main window, the windowed session frame,
    // etc.) -- anything top-level with a caption that isn't a #32770 dialog.
    void ApplyDarkModeToFrame(HWND hwnd) noexcept
    {
        if (!g_darkEnabled || hwnd == nullptr || IsDialogWindow(hwnd))
            return;
        if (!IsTopLevelCaptioned(hwnd) || IsInUntouchableRoot(hwnd))
            return;

        const bool firstTouch = ::GetPropW(hwnd, kReadyProp) == nullptr;
        if (firstTouch && ::SetPropW(hwnd, kReadyProp, reinterpret_cast<HANDLE>(1)) == FALSE)
            return;
        if (!PinHookModule() || !EnsureDarkModeReady())
            return;

        umbra::setDarkTitleBarEx(hwnd, true);
        umbra::enableDarkScrollBarForWindowAndChildren(hwnd);

        if (firstTouch)
        {
            umbra::setWindowEraseBgSubclass(hwnd);
            umbra::setDarkWndNotifySafe(hwnd, true);
            RefreshControls(hwnd);
        }

        if (::GetMenu(hwnd) != nullptr)
        {
            umbra::setWindowMenuBarSubclass(hwnd);
            ::DrawMenuBar(hwnd);
        }

        ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ::RedrawWindow(hwnd, nullptr, nullptr,
                       firstTouch
                           ? RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME | RDW_UPDATENOW
                           : RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
    }

    // ====================================================================
    // WH_CALLWNDPROCRET: theme dialogs AFTER they process WM_INITDIALOG
    // ====================================================================
    // CBT (HCBT_ACTIVATE) themes a dialog's frame and whatever controls exist by
    // then, but dialogs / property-sheet pages that build their contents *during*
    // WM_INITDIALOG (mstsc's expanded "Show Options" tabs) come out half-themed.
    // WH_CALLWNDPROCRET fires AFTER the window proc handles a message, so at
    // WM_INITDIALOG the controls created in the handler already exist -> one
    // ApplyDarkModeToDialog (RefreshControls + subclass) catches them all.
    // (WH_GETMESSAGE would miss it: WM_INITDIALOG is SENT, not posted.)
    LRESULT CALLBACK CwpRetProc(int code, WPARAM wParam, LPARAM lParam) noexcept
    {
        if (code >= 0 && g_darkEnabled && lParam != 0)
        {
            const CWPRETSTRUCT* p = reinterpret_cast<const CWPRETSTRUCT*>(lParam);
            if (p->message == WM_INITDIALOG)
                ApplyDarkModeToDialog(p->hwnd);
        }
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // Installed lazily from the CBT hook, once per GUI thread we see activity on,
    // targeting just that thread -- no extra global injection, the DLL is already
    // in-process. Pins the DLL so CwpRetProc stays valid for the thread's life;
    // the OS drops the thread hook when the thread / process exits.
    thread_local bool t_msgHookInstalled = false;

    void EnsureMessageHookOnThisThread() noexcept
    {
        if (t_msgHookInstalled)
            return;
        t_msgHookInstalled = true;          // one attempt per thread, success or not
        if (!PinHookModule())
            return;
        const HMODULE self = SelfModule();
        if (self != nullptr)
            ::SetWindowsHookExW(WH_CALLWNDPROCRET, CwpRetProc, self, ::GetCurrentThreadId());
    }
}

// ========================================================================
// public entry points (called from hook.cpp, mstsc-only)
// ========================================================================
namespace mstsfence
{
    void DarkModeOnAttach()
    {
        // Governed by the DarkMode setting (HKCU\Software\mstsfence, default on).
        g_darkEnabled = DarkModeEnabled();
    }

    void DarkModeOnCbt(int code, HWND hwnd)
    {
        if (!g_darkEnabled || hwnd == nullptr)
            return;
        EnsureMessageHookOnThisThread();   // arm the WM_INITDIALOG (CWPRETPROC) catch on this thread
        switch (code)
        {
        case HCBT_CREATEWND:
            ApplyDarkModeToFrame(hwnd);   // frame only; controls come at activation
            break;
        case HCBT_ACTIVATE:
            ApplyDarkModeToFrame(hwnd);
            ApplyDarkModeToDialog(hwnd);
            ApplyControl(hwnd);
            break;
        default:
            break;
        }
    }
}
