# mstsfence

Make `mstsc.exe`, in full-screen mode, size itself **to the monitor's work area
(`rcWork`) instead of the whole monitor (`rcMonitor`)** — so the host taskbar
stays visible (two stacked taskbars: host on the bottom, session on top) and the
remote desktop renders **1:1, no scaling, no letterboxing**.

## How it works

Full-screen `mstsc` derives both its window geometry **and** the session
resolution it negotiates (via the Display Control virtual channel / dynamic
resolution) from `GetMonitorInfo`'s `rcMonitor`. So we inline-hook
`GetMonitorInfoW`/`A` inside `mstsc` (via Microsoft Detours) and, on the way out,
collapse `rcMonitor` onto `rcWork`. That single rewrite pins the window above the
taskbar **and** makes `mstsc` request a `3840 × (2160 − taskbar)` session,
tracking DPI/resolution changes live (which a baked `.rdp` cannot). Using
`rcWork` rather than hardcoded numbers means taskbar height, side docking, and
auto-hide all fall out for free.

Staying on stock `mstsc` (rather than hosting the RDP ActiveX control ourselves)
keeps all of its credential UX, Credential Manager integration, and cert/consent
dialogs.

### Getting the hook into mstsc

We **don't control how `mstsc` starts** — `.rdp` files, taskbar jump lists, and
the Start menu all launch it directly — so launch-and-inject is a non-starter.
Instead a small **controller** installs a global `WH_CBT` hook; the OS maps the
hook DLL into GUI processes as they run, catching `mstsc` however it was started.
The DLL filters by exe name, so outside `mstsc` it loads and does nothing.

## Components

| Binary          | Role |
|-----------------|------|
| `mstsfence.exe`     | Tray controller. Installs the global `WH_CBT` hook, survives Explorer restarts, and registers itself to run at login. `--diag` dumps each monitor's `rcMonitor` vs `rcWork`. |
| `mstsfencehook.dll` | Injected hook. Inert except in `mstsc.exe`, where it Detours `GetMonitorInfo` to collapse `rcMonitor` → `rcWork`. |

## Usage

Run `mstsfence.exe` — it lives in the tray (no window), installs the hook, and
registers itself to run at login. Then launch `mstsc` any way you like (`.rdp`,
jump list, Start menu) and go full-screen; it sizes to the work area.

- **Tray menu** (right-click): *Settings* / *About* / *Exit*. **Settings** (stored
  in `HKCU\Software\mstsfence`) has two toggles — fence mstsc to the work area, and
  dark-mode mstsc — both **on by default**; they take effect for mstsc sessions
  started afterward. **Exit** removes the hook and unregisters autostart; a
  close/logoff leaves autostart in place so it returns next login. The icon
  re-appears automatically if Explorer restarts.

```powershell
mstsfence            # run in the tray (default)
mstsfence --diag     # print each monitor's rcMonitor vs rcWork and exit
mstsfence --help
```

An elevated `mstsc` needs an elevated `mstsfence` (a hook can't inject into a
higher-integrity process). Set `MSTSFENCE_TRACE=1` in mstsc's environment to get the
hook's diagnostic log at `%TEMP%\mstsfencehook.log`.

## Dark mode

The hook also dark-styles mstsc's own UI — the connection dialog, the options
tabs, cert/warning prompts, window frames and common controls — via
[umbra](https://github.com/martona/WM_UMBRA) ([src/darkmode.cpp](src/darkmode.cpp)),
driven by the controller's `WH_CBT` hook (frames on `HCBT_CREATEWND`, dialogs +
controls on `HCBT_ACTIVATE`), which in turn installs a thread-local
`WH_CALLWNDPROCRET` to catch `WM_INITDIALOG` *after* the dialog processes it — so
dialogs/property-sheet pages that build their controls late (the expanded Show
Options tabs) get fully themed rather than half-styled. It's on by default and
toggled by the **Dark mode** checkbox in Settings (the `DarkMode` value under
`HKCU\Software\mstsfence`). umbra is pulled in by vcpkg
from the WM_UMBRA **git registry** declared in `src/vcpkg.json`'s
`vcpkg-configuration` — no extra setup, manifest mode resolves it on configure
(the first build clones + builds umbra, so it takes a little longer).

## Building

Requires Visual Studio (C++ toolset), CMake, and vcpkg (for Detours; auto-located
by the build script). Ninja is used if present.

```powershell
./scripts/build_windows.ps1                        # Release; signs if ARTIFACT_SIGNING_* are set
./scripts/build_windows.ps1 -BuildType Debug -DisableCodeSigning
./scripts/build_windows.ps1 -Version 0.2.0.0       # stamp the version resources
```

Output lands in `build/windows-<config>/` (`mstsfence.exe` + `mstsfencehook.dll` side by
side). Signing uses the [`sign`](https://github.com/dotnet/sign) CLI with Azure
Trusted Signing and runs only when `ARTIFACT_SIGNING_ENDPOINT`,
`ARTIFACT_SIGNING_ACCOUNT`, and `ARTIFACT_SIGNING_CERTIFICATE_PROFILE` are all set.

## Status

**Working** on a single-monitor 4K rig: full-screen `mstsc` sizes to the work
area, the host taskbar stays visible, and the session is a true 1:1 `3840×2064`.

`mstsc` reads the screen size from a whole *family* of APIs, so the hook clamps
all of them to the monitor work area: `GetMonitorInfoW/A`, `EnumDisplayMonitors`,
`GetSystemMetrics` (`SM_C*SCREEN` / `SM_C*VIRTUALSCREEN`), and
`EnumDisplaySettingsW`. Hooking `GetMonitorInfo` alone did nothing.

Known limitations / later work:

- **Multi-monitor:** `GetSystemMetrics`/`EnumDisplaySettingsW` clamp to the
  **primary** monitor's work area (single-monitor first cut); per-device mapping
  is TODO. (`GetMonitorInfo`/`EnumDisplayMonitors` are already per-monitor.)
- **Scope:** the clamp applies to every caller inside `mstsc`, not just the
  session window — intentionally left blunt; it works.
- **Injection resilience:** won't apply if `mstsc` ever runs with
  `ProcessExtensionPointDisablePolicy` (blocks window-hook injection) or CIG
  ("Microsoft-signed only"). Neither is on by default.
