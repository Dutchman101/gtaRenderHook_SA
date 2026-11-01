param(
    [string]$GameDir = "",
    [string]$StagingDir = "",
    [string]$X64BuildDir = "",
    [string]$X86BuildDir = "",
    [string]$Config = "RelWithDebInfo",
    [switch]$InstallAsiLoader,
    [string]$AsiLoaderPath = "",
    [switch]$SkipExeHashCheck
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$repoParent = Split-Path -Parent $repoRoot

if (-not $GameDir) {
    $GameDir = Join-Path $repoParent "BaseGameToApplyModsTo"
}
if (-not $StagingDir) {
    $StagingDir = Join-Path $repoParent "build\staging_gtasa"
}
if (-not $X64BuildDir) {
    $X64BuildDir = Join-Path $repoParent "build\vs_x64"
}
if (-not $X86BuildDir) {
    $X86BuildDir = Join-Path $repoParent "build\vs_x86"
}
if (-not $AsiLoaderPath) {
    $AsiLoaderPath = Join-Path $repoParent "build_cache\downloads\ual_winmm\winmm.dll"
}

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string]$FilePath)

    $bytes = [System.IO.File]::ReadAllBytes($FilePath)
    if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
        throw "Not a valid PE binary (missing MZ header): $FilePath"
    }
    $peOffset = [System.BitConverter]::ToInt32($bytes, 0x3C)
    if ($peOffset -lt 0 -or ($peOffset + 6) -gt $bytes.Length) {
        throw "Corrupt PE header offset in: $FilePath"
    }
    $peSig = [System.BitConverter]::ToUInt32($bytes, $peOffset)
    if ($peSig -ne 0x00004550) {
        throw "Missing PE\0\0 signature in: $FilePath"
    }
    return [System.BitConverter]::ToUInt16($bytes, $peOffset + 4)
}

function Resolve-ExistingFile {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string[]]$Candidates
    )

    $existing = @()
    $order = 0
    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            $item = Get-Item -LiteralPath $candidate
            $existing += [PSCustomObject]@{
                Path            = $item.FullName
                LastWriteTimeUtc = $item.LastWriteTimeUtc
                Order           = $order
            }
        }
        $order++
    }
    if ($existing.Count -gt 0) {
        $selected = $existing | Sort-Object -Property @{ Expression = "LastWriteTimeUtc"; Descending = $true }, @{ Expression = "Order"; Ascending = $true } | Select-Object -First 1
        return $selected.Path
    }
    throw "Required artifact '$Label' not found. Checked: $($Candidates -join '; ')"
}

if (-not (Test-Path -LiteralPath $GameDir -PathType Container)) {
    throw "Target GTA SA directory does not exist: $GameDir"
}
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path

$gameExe = Join-Path $GameDir "gta_sa.exe"
if (-not (Test-Path -LiteralPath $gameExe -PathType Leaf)) {
    throw "gta_sa.exe not found in target directory: $GameDir"
}

$expectedSa10UsSha256 = "A559AA772FD136379155EFA71F00C47AAD34BBFEAE6196B0FE1047D0645CBD26"
$actualExeSha256 = (Get-FileHash -LiteralPath $gameExe -Algorithm SHA256).Hash
if (-not $SkipExeHashCheck -and ($actualExeSha256 -ne $expectedSa10UsSha256)) {
    throw "Target gta_sa.exe SHA-256 ($actualExeSha256) does not match GTA SA v1.0 US ($expectedSa10UsSha256). Pass -SkipExeHashCheck only if intentional."
}

# The DirectX 11 ASI (gtaRenderHook.asi) patches overlapping RenderWare/Idle
# addresses and writes an incompatible shaders/ tree; refuse mixed deployment.
$conflictingDx11Paths = @(
    (Join-Path $GameDir "gtaRenderHook.asi"),
    (Join-Path $GameDir "scripts\gtaRenderHook.asi"),
    (Join-Path $GameDir "plugins\gtaRenderHook.asi")
)
foreach ($conflict in $conflictingDx11Paths) {
    if (Test-Path -LiteralPath $conflict -PathType Leaf) {
        throw "Conflicting DirectX 11 plugin detected at '$conflict'. Remove gtaRenderHook.asi before deploying the Vulkan IPC stack."
    }
}

# Snapshot retail base-game files so deployment can verify none were touched.
$protectedRetailFiles = @(
    "gta_sa.exe",
    "vorbisFile.dll",
    "vorbis.dll",
    "ogg.dll",
    "eax.dll",
    "stream.ini"
)
$retailBaselineHashes = @{}
foreach ($name in $protectedRetailFiles) {
    $full = Join-Path $GameDir $name
    if (Test-Path -LiteralPath $full -PathType Leaf) {
        $retailBaselineHashes[$name] = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    }
}

$hookSrc = Resolve-ExistingFile -Label "gta_sa_render_hook.asi (x86)" -Candidates @(
    (Join-Path $X86BuildDir "gta_sa_render_hook\$Config\gta_sa_render_hook.asi"),
    (Join-Path $StagingDir "gta_sa_render_hook.asi"),
    (Join-Path $X86BuildDir "gta_sa_render_hook\gta_sa_render_hook.asi")
)
$driverSrc = Resolve-ExistingFile -Label "gta_sa_render_driver.exe (x64)" -Candidates @(
    (Join-Path $X64BuildDir "gta_sa_render_hook\render_driver\$Config\gta_sa_render_driver.exe"),
    (Join-Path $StagingDir "gta_sa_render_driver.exe"),
    (Join-Path $X64BuildDir "gta_sa_render_hook\render_driver\gta_sa_render_driver.exe")
)
$glslangCandidates = @(
    (Join-Path $X64BuildDir "gta_sa_render_hook\render_driver\$Config\glslangValidator.exe"),
    (Join-Path $StagingDir "glslangValidator.exe")
)
if ($env:VULKAN_SDK) {
    $glslangCandidates += (Join-Path $env:VULKAN_SDK "Bin\glslangValidator.exe")
}
$glslangSrc = Resolve-ExistingFile -Label "glslangValidator.exe (x64)" -Candidates $glslangCandidates

$shadersSrc = Join-Path $repoRoot "shaders"
$resourcesSrc = Join-Path $repoRoot "rw_rh_engine_lib\resources"
$blueNoiseSrc = Join-Path $resourcesSrc "blue_noise.bmp"
if (-not (Test-Path -LiteralPath $shadersSrc -PathType Container)) {
    throw "Shader source directory missing: $shadersSrc"
}
if (-not (Test-Path -LiteralPath $blueNoiseSrc -PathType Leaf)) {
    throw "Runtime resource missing: $blueNoiseSrc"
}

$hookMachine = Get-PeMachine -FilePath $hookSrc
if ($hookMachine -ne 0x014C) {
    throw ("gta_sa_render_hook.asi must be 32-bit x86 (0x014C), got 0x{0:X4} at {1}" -f $hookMachine, $hookSrc)
}
$driverMachine = Get-PeMachine -FilePath $driverSrc
if ($driverMachine -ne 0x8664) {
    throw ("gta_sa_render_driver.exe must be 64-bit x64 (0x8664), got 0x{0:X4} at {1}" -f $driverMachine, $driverSrc)
}
$glslangMachine = Get-PeMachine -FilePath $glslangSrc
if ($glslangMachine -ne 0x8664) {
    throw ("glslangValidator.exe must be 64-bit x64 (0x8664), got 0x{0:X4} at {1}" -f $glslangMachine, $glslangSrc)
}

if ($InstallAsiLoader) {
    if (-not (Test-Path -LiteralPath $AsiLoaderPath -PathType Leaf)) {
        $dlDir = Split-Path -Parent $AsiLoaderPath
        New-Item -ItemType Directory -Force -Path $dlDir | Out-Null
        $zipPath = Join-Path $dlDir "Ultimate-ASI-Loader_Win32.zip"
        $shaPath = Join-Path $dlDir "winmm-Win32.SHA512"
        $baseUrl = "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/Win32-latest"
        Invoke-WebRequest -Uri "$baseUrl/Ultimate-ASI-Loader_Win32.zip" -OutFile $zipPath
        Invoke-WebRequest -Uri "$baseUrl/winmm-Win32.SHA512" -OutFile $shaPath
        Expand-Archive -LiteralPath $zipPath -DestinationPath $dlDir -Force
        $expectedSha512 = ((Get-Content -LiteralPath $shaPath -Raw).Trim() -split '\s+')[0].ToUpperInvariant()
        $actualSha512 = (Get-FileHash -LiteralPath $AsiLoaderPath -Algorithm SHA512).Hash.ToUpperInvariant()
        if ($actualSha512 -ne $expectedSha512) {
            Remove-Item -LiteralPath $AsiLoaderPath -Force -ErrorAction SilentlyContinue
            throw "Ultimate ASI Loader SHA-512 mismatch: expected $expectedSha512, got $actualSha512"
        }
    }
    $ualMachine = Get-PeMachine -FilePath $AsiLoaderPath
    if ($ualMachine -ne 0x014C) {
        throw ("Ultimate ASI Loader winmm.dll must be 32-bit x86 (0x014C), got 0x{0:X4}" -f $ualMachine)
    }
    Copy-Item -LiteralPath $AsiLoaderPath -Destination (Join-Path $GameDir "winmm.dll") -Force
}

# Both 32-bit gta_sa_render_hook.asi and 64-bit gta_sa_render_driver.exe share
# the game folder and import vulkan-1.dll; remove any single-bitness app-local
# copy so Windows loads SysWOW64\vulkan-1.dll and System32\vulkan-1.dll.
$appLocalVulkan = Join-Path $GameDir "vulkan-1.dll"
if (Test-Path -LiteralPath $appLocalVulkan -PathType Leaf) {
    Remove-Item -LiteralPath $appLocalVulkan -Force
    Write-Host "[CLEAN] Removed app-local vulkan-1.dll to preserve WOW64 SysWOW64/System32 loader routing."
}

Copy-Item -LiteralPath $hookSrc -Destination (Join-Path $GameDir "gta_sa_render_hook.asi") -Force
Copy-Item -LiteralPath $driverSrc -Destination (Join-Path $GameDir "gta_sa_render_driver.exe") -Force
Copy-Item -LiteralPath $glslangSrc -Destination (Join-Path $GameDir "glslangValidator.exe") -Force

$destShaders = Join-Path $GameDir "shaders"
$destResources = Join-Path $GameDir "resources"
New-Item -ItemType Directory -Force -Path $destShaders | Out-Null
New-Item -ItemType Directory -Force -Path $destResources | Out-Null

Get-ChildItem -LiteralPath $shadersSrc -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $destShaders -Recurse -Force
}
Get-ChildItem -LiteralPath $resourcesSrc -Force | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $destResources -Recurse -Force
}

foreach ($entry in $retailBaselineHashes.GetEnumerator()) {
    $full = Join-Path $GameDir $entry.Key
    $postHash = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    if ($postHash -ne $entry.Value) {
        throw "Retail file integrity check failed for '$($entry.Key)': expected $($entry.Value), got $postHash"
    }
}

$deployedWinmm = Join-Path $GameDir "winmm.dll"
$hasAsiLoader = (Test-Path -LiteralPath $deployedWinmm -PathType Leaf)

Write-Host "[OK] Deployed RenderHook Vulkan IPC package to: $GameDir"
Write-Host ("     gta_sa_render_hook.asi   (PE 0x{0:X4}) <- {1}" -f (Get-PeMachine -FilePath (Join-Path $GameDir "gta_sa_render_hook.asi")), $hookSrc)
Write-Host ("     gta_sa_render_driver.exe (PE 0x{0:X4}) <- {1}" -f (Get-PeMachine -FilePath (Join-Path $GameDir "gta_sa_render_driver.exe")), $driverSrc)
Write-Host ("     glslangValidator.exe     (PE 0x{0:X4}) <- {1}" -f (Get-PeMachine -FilePath (Join-Path $GameDir "glslangValidator.exe")), $glslangSrc)
if ($hasAsiLoader) {
    Write-Host ("     winmm.dll (ASI Loader)   (PE 0x{0:X4})" -f (Get-PeMachine -FilePath $deployedWinmm))
} else {
    Write-Warning "No winmm.dll ASI loader detected in $GameDir. Re-run with -InstallAsiLoader if no ASI loader is installed."
}
Write-Host "     Retail files verified untouched: $($retailBaselineHashes.Count) file(s)."
