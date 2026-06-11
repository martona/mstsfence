#!/usr/bin/env pwsh
<#
.SYNOPSIS
  Packages the built mstsfence Win32 binaries as a full-trust MSIX.

.DESCRIPTION
  Stages mstsfence.exe + mstsfencehook.dll, generates the PNG logo set and
  resources.pri index, fills in the AppxManifest template, runs makeappx, and
  optionally signs the package with Azure Trusted Signing.

  Signing uses the same ARTIFACT_SIGNING_* environment variables as
  build_windows.ps1. Use -NoSign for CI or Store-style flows where the produced
  .msix is signed by a later step.

  The manifest declares rescap:unvirtualizedResources so the existing HKCU
  autostart/settings writes and the staged hook DLL under %APPDATA% are visible
  to unpackaged processes such as mstsc.exe.

.NOTES
  Requires the Windows 10/11 SDK (makeappx.exe and makepri.exe). Local signing
  also requires sign.exe:

    dotnet tool install --global --prerelease sign

  Build the matching arch first, for example:

    .\scripts\build_windows.ps1 -BuildType Release -Triplet x64-windows-static -VcVarsArch amd64
#>
[CmdletBinding()]
param(
    [ValidateSet('amd64', 'arm64')]
    [string]$Arch = 'amd64',

    # MSIX version (W.X.Y.Z). Omit to read it from mstsfence.exe VERSIONINFO.
    # For Microsoft Store submission, the 4th field is reserved and should be 0.
    [string]$Version = '',

    [string]$BuildDir = 'build\windows-release',
    [string]$OutDir = 'build\msix',

    # Package identity. Publisher must match the certificate used to sign the
    # MSIX. If mstsfence.exe is already signed, the script reads its subject and
    # uses that automatically.
    [string]$IdentityName = 'MartonAnka.mstsfence',
    [string]$Publisher = '',
    [string]$PublisherDisplayName = 'Marton Anka',

    # Pack only, leaving the .msix unsigned.
    [switch]$NoSign
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $repoRoot
[Environment]::CurrentDirectory = $repoRoot

function Resolve-RepoPath {
    param([Parameter(Mandatory)][string]$Path)

    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }

    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Assert-ChildPath {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Parent
    )

    $fullPath = [IO.Path]::GetFullPath($Path)
    $fullParent = [IO.Path]::GetFullPath($Parent)
    if (-not $fullParent.EndsWith([IO.Path]::DirectorySeparatorChar)) {
        $fullParent += [IO.Path]::DirectorySeparatorChar
    }

    if (-not $fullPath.StartsWith($fullParent, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to operate on '$fullPath' because it is not under '$fullParent'."
    }
}

function Find-SdkTool {
    param([Parameter(Mandatory)][string]$Name)

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }

    $kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (-not (Test-Path $kitsBin)) {
        throw "Windows SDK not found at '$kitsBin'. Install the Windows 10/11 SDK."
    }

    $hostArch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { 'x64' }
    $versions = Get-ChildItem -Path $kitsBin -Directory |
        Where-Object { $_.Name -like '10.*' } |
        Sort-Object Name -Descending

    foreach ($versionDir in $versions) {
        foreach ($toolArch in @($hostArch, 'x64', 'x86')) {
            $candidate = Join-Path $versionDir.FullName "$toolArch\$Name"
            if (Test-Path $candidate) {
                return $candidate
            }
        }
    }

    throw "Could not locate $Name under '$kitsBin'."
}

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath' failed with exit code $LASTEXITCODE."
    }
}

function Import-DrawingAssembly {
    if ('System.Drawing.Bitmap' -as [type]) {
        return
    }

    try {
        Add-Type -AssemblyName System.Drawing -ErrorAction Stop
    }
    catch {
        try {
            Add-Type -AssemblyName System.Drawing.Common -ErrorAction Stop
        }
        catch {
            throw "System.Drawing is unavailable in this PowerShell. Run the script on Windows with System.Drawing available."
        }
    }
}

function New-LogoPng {
    param(
        [Parameter(Mandatory)][string]$Master,
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][int]$Size
    )

    $src = [System.Drawing.Image]::FromFile($Master)
    try {
        $dst = New-Object System.Drawing.Bitmap $Size, $Size
        try {
            $graphics = [System.Drawing.Graphics]::FromImage($dst)
            try {
                $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
                $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
                $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
                $graphics.Clear([System.Drawing.Color]::Transparent)

                $scale = [Math]::Min($Size / $src.Width, $Size / $src.Height)
                $drawWidth = [int][Math]::Round($src.Width * $scale, [MidpointRounding]::AwayFromZero)
                $drawHeight = [int][Math]::Round($src.Height * $scale, [MidpointRounding]::AwayFromZero)
                $drawX = [int][Math]::Floor(($Size - $drawWidth) / 2)
                $drawY = [int][Math]::Floor(($Size - $drawHeight) / 2)
                $graphics.DrawImage($src, $drawX, $drawY, $drawWidth, $drawHeight)
            }
            finally {
                $graphics.Dispose()
            }

            $dst.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
        }
        finally {
            $dst.Dispose()
        }
    }
    finally {
        $src.Dispose()
    }
}

$BuildDir = Resolve-RepoPath $BuildDir
$OutDir = Resolve-RepoPath $OutDir
$masterImage = Join-Path $repoRoot 'resources\fence-icon.png'
$manifestTemplate = Join-Path $repoRoot 'src\msix\AppxManifest.xml.in'
$msixArch = if ($Arch -eq 'amd64') { 'x64' } else { 'arm64' }

if (-not $NoSign) {
    if (-not (Get-Command sign.exe -ErrorAction SilentlyContinue)) {
        throw "sign.exe is not on PATH. Install it with: dotnet tool install --global --prerelease sign"
    }

    foreach ($name in 'ARTIFACT_SIGNING_ENDPOINT', 'ARTIFACT_SIGNING_ACCOUNT', 'ARTIFACT_SIGNING_CERTIFICATE_PROFILE') {
        if (-not [Environment]::GetEnvironmentVariable($name)) {
            throw "Signing requires $name. Pass -NoSign to pack without signing."
        }
    }
}

$makeappx = Find-SdkTool 'makeappx.exe'
$makepri = Find-SdkTool 'makepri.exe'
Write-Host "[*] makeappx: $makeappx"
Write-Host "[*] makepri:  $makepri"

$appExe = Join-Path $BuildDir 'mstsfence.exe'
$hookDll = Join-Path $BuildDir 'mstsfencehook.dll'
if (-not (Test-Path $appExe)) {
    $triplet = if ($Arch -eq 'amd64') { 'x64-windows-static' } else { 'arm64-windows-static' }
    throw "mstsfence.exe not found at '$appExe'. Build first: .\scripts\build_windows.ps1 -BuildType Release -Triplet $triplet -VcVarsArch $Arch"
}
if (-not (Test-Path $hookDll)) {
    throw "mstsfencehook.dll not found at '$hookDll'. Build mstsfence before packaging."
}
if (-not (Test-Path $masterImage)) {
    throw "Master app image not found: $masterImage"
}
if (-not (Test-Path $manifestTemplate)) {
    throw "Missing manifest template: $manifestTemplate"
}

if (-not $Version) {
    $versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($appExe)
    $Version = '{0}.{1}.{2}.{3}' -f $versionInfo.FileMajorPart, $versionInfo.FileMinorPart, $versionInfo.FileBuildPart, $versionInfo.FilePrivatePart
    Write-Host "[*] Version (from mstsfence.exe): $Version"
}
if ($Version -notmatch '^\d+\.\d+\.\d+\.\d+$') {
    throw "MSIX Version must be W.X.Y.Z (4 integer components); got '$Version'."
}

$signature = Get-AuthenticodeSignature $appExe -ErrorAction SilentlyContinue
if ($signature -and $signature.SignerCertificate -and $signature.Status -ne 'NotSigned') {
    $detectedPublisher = $signature.SignerCertificate.Subject
    if ($Publisher -and $Publisher -ne $detectedPublisher) {
        Write-Warning "Using signed mstsfence.exe publisher '$detectedPublisher' instead of '$Publisher'. MSIX Publisher must match the signing certificate subject."
    }
    $Publisher = $detectedPublisher
    Write-Host "[*] Publisher (from signed mstsfence.exe): $Publisher"
}
elseif (-not $Publisher) {
    if ($NoSign) {
        $Publisher = 'CN=mstsfence'
        Write-Warning "mstsfence.exe is unsigned; using placeholder Publisher '$Publisher'. Sign with a matching certificate before installing."
    }
    else {
        throw "mstsfence.exe is unsigned, so the signing certificate subject cannot be auto-detected. Sign it first or pass -Publisher with the exact certificate subject."
    }
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$layout = Join-Path $OutDir "layout-$Arch"
Assert-ChildPath -Path $layout -Parent $OutDir
if (Test-Path $layout) {
    Remove-Item -LiteralPath $layout -Recurse -Force
}

$layoutImages = Join-Path $layout 'Images'
New-Item -ItemType Directory -Force -Path $layoutImages | Out-Null

Copy-Item -LiteralPath $appExe -Destination (Join-Path $layout 'mstsfence.exe') -Force
Copy-Item -LiteralPath $hookDll -Destination (Join-Path $layout 'mstsfencehook.dll') -Force

Import-DrawingAssembly
$scales = @(125, 150, 200, 400)
$squareLogos = [ordered]@{
    'StoreLogo'         = 50
    'Square44x44Logo'   = 44
    'Square71x71Logo'   = 71
    'Square150x150Logo' = 150
}

foreach ($name in $squareLogos.Keys) {
    $baseSize = $squareLogos[$name]
    New-LogoPng -Master $masterImage -Path (Join-Path $layoutImages "$name.png") -Size $baseSize

    foreach ($scale in $scales) {
        $pixels = [int][Math]::Round($baseSize * $scale / 100.0, [MidpointRounding]::AwayFromZero)
        New-LogoPng -Master $masterImage -Path (Join-Path $layoutImages "$name.scale-$scale.png") -Size $pixels
    }
}

foreach ($targetSize in @(16, 24, 32, 48, 256)) {
    New-LogoPng -Master $masterImage -Path (Join-Path $layoutImages "Square44x44Logo.targetsize-$targetSize.png") -Size $targetSize
    New-LogoPng -Master $masterImage -Path (Join-Path $layoutImages "Square44x44Logo.targetsize-${targetSize}_altform-unplated.png") -Size $targetSize
}
Write-Host "[*] Generated $((Get-ChildItem -Path $layoutImages -Filter *.png).Count) logo PNGs in $layoutImages"

$manifest = (Get-Content -Raw $manifestTemplate).
    Replace('@IDENTITY_NAME@', $IdentityName).
    Replace('@PUBLISHER@', $Publisher).
    Replace('@PUBLISHER_DISPLAY_NAME@', $PublisherDisplayName).
    Replace('@VERSION@', $Version).
    Replace('@ARCH@', $msixArch)
Set-Content -Path (Join-Path $layout 'AppxManifest.xml') -Value $manifest -Encoding UTF8

$priConfig = Join-Path $OutDir 'priconfig.xml'
Invoke-NativeCommand -FilePath $makepri -Arguments @('createconfig', '/cf', $priConfig, '/dq', 'en-US', '/o')
Invoke-NativeCommand -FilePath $makepri -Arguments @('new', '/pr', $layout, '/cf', $priConfig, '/mn', (Join-Path $layout 'AppxManifest.xml'), '/of', (Join-Path $layout 'resources.pri'), '/o')

$msix = Join-Path $OutDir "mstsfence-windows-$Arch.msix"
if (Test-Path $msix) {
    Remove-Item -LiteralPath $msix -Force
}
Invoke-NativeCommand -FilePath $makeappx -Arguments @('pack', '/d', $layout, '/p', $msix, '/o')
Write-Host "[*] Packed: $msix"

if ($NoSign) {
    Write-Host '[*] -NoSign: leaving the .msix unsigned.'
}
else {
    $signArgs = @(
        'code', 'artifact-signing',
        '-b', $OutDir,
        '-ase', $env:ARTIFACT_SIGNING_ENDPOINT,
        '-asa', $env:ARTIFACT_SIGNING_ACCOUNT,
        (Split-Path -Leaf $msix),
        '-v', 'Information',
        '-ascp', $env:ARTIFACT_SIGNING_CERTIFICATE_PROFILE
    )
    Invoke-NativeCommand -FilePath 'sign.exe' -Arguments $signArgs
    Write-Host '[*] Signed with Azure Trusted Signing.'
}

Write-Host ''
Write-Host "[*] Done: $msix"
Write-Host "[*] Logo PNGs left for inspection in: $layoutImages"

if ($NoSign) {
    Write-Host '[*] Package is unsigned. Sign it before install or distribution.'
}
else {
    Write-Host ''
    Write-Host 'Sideload-test:'
    Write-Host "  Add-AppxPackage '$msix'"
}

Write-Host ''
Write-Host 'MSIX note: manifest declares the restricted unvirtualizedResources'
Write-Host 'capability. Registry writes go to real HKCU, and packaged builds stage'
Write-Host 'mstsfencehook.dll under real %APPDATA%\mstsfence for mstsc.exe to read.'
