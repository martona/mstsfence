# mstsfence

Make `mstsc.exe`, in full-screen mode, size itself to the monitor's work area 
instead of the whole monitor - so the host taskbar
stays visible (two stacked taskbars: host on the bottom, session on top) and the
remote desktop renders 1:1, no scaling, no letterboxing.

## How it works

Full-screen `mstsc` derives both its window geometry and the session
resolution it negotiates from `GetMonitorInfo`'s `rcMonitor`. So we inline-hook
`GetMonitorInfoW`/`A` inside `mstsc` (via Microsoft Detours) and, on the way out,
collapse `rcMonitor` onto `rcWork`. That single rewrite pins the window above the
taskbar and makes `mstsc` request a `3840 × (2160 − taskbar)` session,
tracking DPI/resolution changes live (which a baked `.rdp` cannot). Using
`rcWork` rather than hardcoded numbers means taskbar height, side docking, and
auto-hide all fall out for free.

Staying on stock `mstsc` (rather than hosting the RDP ActiveX control ourselves)
keeps all of its credential UX, Credential Manager integration, and cert/consent
dialogs.

NOTE: Relying on `GetMonitorInfo` alone didn't prove to be sufficient so we also hook 
`EnumDisplayMonitors`, `GetSystemMetrics`, and `EnumDisplaySettingsW`.

### DPI / host scaling override

`mstsfence` can also ask the remote host to use a chosen display scaling
percentage. When **DPI override** is enabled, the hook rewrites the RDP monitor
scale data in two places:

- connect-time `CS_MONITOR` / `CS_MONITOR_EX` data, including the single-monitor
  case where stock `mstsc` normally sends only `CS_CORE` dimensions and no monitor
  attributes;
- later dynamic-display `DISPLAYCONTROL_MONITOR_LAYOUT` PDUs, so reconnects,
  resizes, and display-control traffic keep carrying the selected scale.

This part is version-dependent: it hooks private `mstscax.dll` functions by RVA,
so it only runs on builds whose PDB GUID is known. The Settings dialog shows a
small **RVA** status readout so you can quickly tell whether the installed
`mstscax.dll` is supported.

### Getting the hook into mstsc

We don't control how `mstsc` starts - `.rdp` files, taskbar jump lists, and
the Start menu all launch it directly - so launch-and-inject is a non-starter.
Instead a small controller installs a global `WH_CBT` hook; the OS maps the
hook DLL into GUI processes as they run, catching `mstsc` however it was started.
The DLL filters by exe name, so outside `mstsc` it loads and does nothing.

## Components

| Binary          | Role |
|-----------------|------|
| `mstsfence.exe`     | Tray controller. Installs the global `WH_CBT` hook, and registers itself to run at login. |
| `mstsfencehook.dll` | Injected hook. Inert except in `mstsc.exe`, where it clamps monitor APIs to the work area and, when enabled, rewrites RDP monitor DPI data. |

## Usage

Run `mstsfence.exe` — it lives in the tray (no window), installs the hook, and
registers itself to run at login. Then launch `mstsc` any way you like (`.rdp`,
jump list, Start menu) and go full-screen; it sizes to the work area.

- **Tray menu** (right-click): *Settings* / *About* / *Exit*. **Settings** (stored
  in `HKCU\Software\mstsfence`) has toggles for fencing mstsc to the work area
  and dark-mode mstsc — both **on by default** — plus an optional DPI override
  percentage for the remote host. Changes take effect for mstsc sessions started
  afterward. The small RVA readout in the lower-right of Settings reports whether
  the installed `mstscax.dll` build matches the private offsets needed by the DPI
  override. **Exit** removes the hook and unregisters autostart; a close/logoff
  leaves autostart in place so it returns next login. The icon re-appears
  automatically if Explorer restarts.

An elevated `mstsc` needs an elevated `mstsfence` (a hook can't inject into a
higher-integrity process). Set `MSTSFENCE_TRACE=1` in mstsc's environment to get the
hook's diagnostic log at `%TEMP%\mstsfencehook.log`.

## Building

Requires Visual Studio (C++ toolset), CMake, and vcpkg (for Detours; auto-located
by the build script). Ninja is used if present.

```powershell
./scripts/build_windows.ps1                        # Release; signs if ARTIFACT_SIGNING_* are set
./scripts/build_windows.ps1 -BuildType Debug -DisableCodeSigning
./scripts/build_windows.ps1 -Version 0.1.0.1       # stamp a specific version
```

Output lands in `build/windows-<config>/` (`mstsfence.exe` + `mstsfencehook.dll` side by
side). Signing uses the [`sign`](https://github.com/dotnet/sign) CLI with Azure
Trusted Signing and runs only when `ARTIFACT_SIGNING_ENDPOINT`,
`ARTIFACT_SIGNING_ACCOUNT`, and `ARTIFACT_SIGNING_CERTIFICATE_PROFILE` are all set.

To package an already-built Release directory as an MSIX:

```powershell
./scripts/package_windows_msix.ps1 -NoSign
```

The packager stages the EXE and hook DLL, generates MSIX PNG assets from
`resources/fence-icon.png`, builds `resources.pri`, and emits
`build/msix/mstsfence-windows-<arch>.msix`. Omit `-NoSign` to sign the
MSIX with the same Azure Trusted Signing environment variables used by the build
script; if the EXE is already signed, the manifest Publisher is inferred from it.
The package declares the restricted `unvirtualizedResources` capability so the
existing HKCU autostart/settings writes land in the real registry, and packaged
builds stage `mstsfencehook.dll` under `%APPDATA%\mstsfence` for `mstsc.exe` to
load. The release workflow builds and signs MSIX packages when Azure Trusted
Signing is configured.

## Releasing

Releases are built by GitHub Actions (`.github/workflows/`), `amd64` + `arm64`.
Push a `vW.X.Y.Z` tag and the pipeline builds both arches, signs them (Azure
Trusted Signing, when the signing vars/secrets are configured), and opens a
**draft** release:

```powershell
git tag v0.1.0.1
git push origin v0.1.0.1
```

Or run **Release (manual)** from the Actions tab with an explicit version (tick
*publish* to release immediately instead of drafting). Either way each release
carries, per architecture:

- `mstsfence-windows-<arch>.zip` — the loose `mstsfence.exe` + `mstsfencehook.dll`.
- `mstsfence-windows-<arch>.msix` — the signed full-trust MSIX package (when
  Azure Trusted Signing is configured).
- `mstsfence-<version>-windows-<arch>-symbols.zip` — the matching `.pdb` symbols.

Every push/PR to `master` also runs **Windows CI**, which builds both arches and
asserts the binaries pull in no VC++ runtime DLLs (the static-CRT invariant).

## Status

Working on a triple-monitor 4K rig: full-screen `mstsc` sizes to the work
area, the host taskbar stays visible, and the session is a true 1:1 `3840×2064`.

`mstsc` reads the screen size from a whole *family* of APIs, so the hook clamps
all of them to the monitor work area: `GetMonitorInfoW/A`, `EnumDisplayMonitors`,
`GetSystemMetrics` (`SM_C*SCREEN` / `SM_C*VIRTUALSCREEN`), and
`EnumDisplaySettingsW`. Hooking `GetMonitorInfo` alone did nothing.

Known limitations / later work:

- **Multi-monitor:** `GetSystemMetrics`/`EnumDisplaySettingsW` clamp to the
  **primary** monitor's work area (single-monitor first cut); per-device mapping
  is TODO. (`GetMonitorInfo`/`EnumDisplayMonitors` are already per-monitor.)
- **DPI override build coverage:** the override depends on private `mstscax.dll`
  RVAs and is therefore gated by known PDB GUIDs. Unsupported builds safely leave
  the DPI path alone; check the Settings dialog's RVA readout if scaling does not
  change.
- **Scope:** the clamp applies to every caller inside `mstsc`, not just the
  session window — intentionally left blunt; it works.
- **Injection resilience:** won't apply if `mstsc` ever runs with
  `ProcessExtensionPointDisablePolicy` (blocks window-hook injection) or CIG
  ("Microsoft-signed only"). Neither is on by default.
