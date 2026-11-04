param(
    [string]$GameDir = "",
    [string]$StagingDir = "",
    [string]$BuildDir = "",
    [string]$Config = "Release",
    [switch]$InstallAsiLoader,
    [string]$AsiLoaderPath = "",
    [switch]$ShadersOnly,
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
if (-not $BuildDir) {
    $BuildDir = Join-Path $repoParent "build\vs_x86"
}
if (-not $AsiLoaderPath) {
    $AsiLoaderPath = Join-Path $repoParent "build_cache\downloads\ual_winmm\winmm.dll"
}

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string]$FilePath)

    $stream = [System.IO.File]::Open($FilePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -lt 64) {
            throw "Not a valid PE binary (file shorter than 64-byte DOS header): $FilePath"
        }
        $dosHeader = New-Object byte[] 64
        $readDos = $stream.Read($dosHeader, 0, 64)
        if ($readDos -lt 64 -or $dosHeader[0] -ne 0x4D -or $dosHeader[1] -ne 0x5A) {
            throw "Not a valid PE binary (missing MZ header): $FilePath"
        }
        $peOffset = [System.BitConverter]::ToInt32($dosHeader, 0x3C)
        if ($peOffset -lt 0 -or ([long]$peOffset + 6) -gt $stream.Length) {
            throw "Corrupt PE header offset in: $FilePath"
        }
        [void]$stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin)
        $peHeader = New-Object byte[] 6
        $readPe = $stream.Read($peHeader, 0, 6)
        if ($readPe -lt 6 -or ([System.BitConverter]::ToUInt32($peHeader, 0) -ne 0x00004550)) {
            throw "Missing PE\0\0 signature in: $FilePath"
        }
        return [System.BitConverter]::ToUInt16($peHeader, 4)
    } finally {
        $stream.Dispose()
    }
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
                Path             = $item.FullName
                LastWriteTimeUtc = $item.LastWriteTimeUtc
                Order            = $order
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

$shadersSrc = Join-Path $repoRoot "gtaRenderHook\shaders"
if (-not (Test-Path -LiteralPath $shadersSrc -PathType Container)) {
    throw "DirectX 11 shader source directory missing: $shadersSrc"
}

$conflictingAsiPaths = @(
    (Join-Path $GameDir "gtaRenderHook_SA.asi"),
    (Join-Path $GameDir "_gtaRenderHook.asi"),
    (Join-Path $GameDir "scripts\gtaRenderHook.asi"),
    (Join-Path $GameDir "scripts\gtaRenderHook_SA.asi"),
    (Join-Path $GameDir "scripts\_gtaRenderHook.asi"),
    (Join-Path $GameDir "scripts\gta_sa_render_hook.asi"),
    (Join-Path $GameDir "plugins\gtaRenderHook.asi"),
    (Join-Path $GameDir "plugins\gtaRenderHook_SA.asi"),
    (Join-Path $GameDir "plugins\_gtaRenderHook.asi"),
    (Join-Path $GameDir "plugins\gta_sa_render_hook.asi")
)
$foundConflicts = @($conflictingAsiPaths | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf })
if ($foundConflicts.Count -gt 0) {
    throw "Conflicting RenderHook ASI file(s) found (ASI Stack Mutual Exclusion violation): $($foundConflicts -join ', '). Remove them before deploying."
}

$hookPdbSrc = $null
$atbDllSrc = $null
if (-not $ShadersOnly) {
    $hookSrc = Resolve-ExistingFile -Label "gtaRenderHook.asi (x86 DirectX 11)" -Candidates @(
        (Join-Path $BuildDir "$Config\gtaRenderHook.asi"),
        (Join-Path $BuildDir "gtaRenderHook\$Config\gtaRenderHook.asi"),
        (Join-Path $BuildDir "gtaRenderHook.asi"),
        (Join-Path $BuildDir "gtaRenderHook\gtaRenderHook.asi"),
        (Join-Path $StagingDir "gtaRenderHook.asi"),
        (Join-Path $repoRoot "gtaRenderHook\$Config\gtaRenderHook.asi"),
        (Join-Path $repoRoot "$Config\gtaRenderHook.asi")
    )
    $hookMachine = Get-PeMachine -FilePath $hookSrc
    if ($hookMachine -ne 0x014C) {
        throw ("gtaRenderHook.asi must be 32-bit x86 (0x014C), got 0x{0:X4} at {1}" -f $hookMachine, $hookSrc)
    }
    $pdbCandidate = [System.IO.Path]::ChangeExtension($hookSrc, ".pdb")
    if (Test-Path -LiteralPath $pdbCandidate -PathType Leaf) {
        $hookPdbSrc = $pdbCandidate
    }
    $atbCandidates = @(@(
        $(if ($env:ANT_TWEAKBAR) { Join-Path $env:ANT_TWEAKBAR "lib\AntTweakBar.dll" }),
        $(if ($env:ANT_TWEAKBAR_DIR) { Join-Path $env:ANT_TWEAKBAR_DIR "lib\AntTweakBar.dll" }),
        (Join-Path $repoParent "build_cache\deps\AntTweakBar\lib\AntTweakBar.dll")
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) })
    if ($atbCandidates.Count -gt 0) {
        $atbDllSrc = $atbCandidates[0]
        $atbMachine = Get-PeMachine -FilePath $atbDllSrc
        if ($atbMachine -ne 0x014C) {
            throw ("AntTweakBar.dll must be 32-bit x86 (0x014C), got 0x{0:X4} at {1}" -f $atbMachine, $atbDllSrc)
        }
    }
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
} else {
    $existingWinmm = Join-Path $GameDir "winmm.dll"
    if (Test-Path -LiteralPath $existingWinmm -PathType Leaf) {
        $existingWinmmMachine = Get-PeMachine -FilePath $existingWinmm
        if ($existingWinmmMachine -ne 0x014C) {
            throw ("Existing winmm.dll in $GameDir must be 32-bit x86 (0x014C), got 0x{0:X4}" -f $existingWinmmMachine)
        }
    }
}

$legacyVulkanFiles = @(
    "gta_sa_render_hook.asi",
    "gta_sa_render_hook.pdb",
    "gta_sa_render_driver.exe",
    "gta_sa_render_driver.pdb",
    "glslangValidator.exe",
    "vulkan-1.dll"
)
if (-not $ShadersOnly -or (Test-Path -LiteralPath (Join-Path $GameDir "gtaRenderHook.asi") -PathType Leaf)) {
    foreach ($legacyName in $legacyVulkanFiles) {
        $legacyPath = Join-Path $GameDir $legacyName
        if (Test-Path -LiteralPath $legacyPath -PathType Leaf) {
            Remove-Item -LiteralPath $legacyPath -Force
            Write-Host "     Removed retired Vulkan IPC artifact: $legacyName"
        }
    }
} elseif (Test-Path -LiteralPath (Join-Path $GameDir "gta_sa_render_hook.asi") -PathType Leaf) {
    Write-Warning "Retired Vulkan IPC plugin 'gta_sa_render_hook.asi' is still present in $GameDir. Run deploy_to_game.ps1 without -ShadersOnly after building gtaRenderHook.asi to replace it."
}
foreach ($legacyShaderSubdir in @("vulkan", "d3d11")) {
    $legacyDirPath = Join-Path $GameDir "shaders\$legacyShaderSubdir"
    if (Test-Path -LiteralPath $legacyDirPath -PathType Container) {
        Remove-Item -LiteralPath $legacyDirPath -Recurse -Force
        Write-Host "     Removed retired shader directory: shaders\$legacyShaderSubdir"
    }
}

if (-not $ShadersOnly) {
    Copy-Item -LiteralPath $hookSrc -Destination (Join-Path $GameDir "gtaRenderHook.asi") -Force
    $destPdb = Join-Path $GameDir "gtaRenderHook.pdb"
    if ($hookPdbSrc) {
        Copy-Item -LiteralPath $hookPdbSrc -Destination $destPdb -Force
    } elseif (Test-Path -LiteralPath $destPdb -PathType Leaf) {
        Remove-Item -LiteralPath $destPdb -Force
        Write-Host "     Removed stale symbol file: gtaRenderHook.pdb"
    }
    if ($atbDllSrc) {
        Copy-Item -LiteralPath $atbDllSrc -Destination (Join-Path $GameDir "AntTweakBar.dll") -Force
    }
}

$destShaders = Join-Path $GameDir "shaders"
New-Item -ItemType Directory -Force -Path $destShaders | Out-Null

Get-ChildItem -LiteralPath $shadersSrc -File -Force | Where-Object { $_.Extension -in @('.hlsl', '.hlsli') } | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $destShaders $_.Name) -Force
}

foreach ($entry in $retailBaselineHashes.GetEnumerator()) {
    $full = Join-Path $GameDir $entry.Key
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) {
        throw "Protected retail file '$($entry.Key)' is missing after deployment: $full"
    }
    $postHash = (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash
    if ($postHash -ne $entry.Value) {
        throw "Retail file integrity check failed for '$($entry.Key)': expected $($entry.Value), got $postHash"
    }
}

$deployedWinmm = Join-Path $GameDir "winmm.dll"
$hasAsiLoader = (Test-Path -LiteralPath $deployedWinmm -PathType Leaf)
$deployedWinmmMachine = $null
if ($hasAsiLoader) {
    $deployedWinmmMachine = Get-PeMachine -FilePath $deployedWinmm
    if ($deployedWinmmMachine -ne 0x014C) {
        throw ("Existing winmm.dll in $GameDir must be 32-bit x86 (0x014C), got 0x{0:X4}" -f $deployedWinmmMachine)
    }
}

Write-Host "[OK] Deployed RenderHook DirectX 11 package to: $GameDir"
if (-not $ShadersOnly) {
    Write-Host ("     gtaRenderHook.asi      (PE 0x{0:X4}) <- {1}" -f (Get-PeMachine -FilePath (Join-Path $GameDir "gtaRenderHook.asi")), $hookSrc)
    if ($hookPdbSrc) {
        Write-Host ("     gtaRenderHook.pdb                   <- {0}" -f $hookPdbSrc)
    }
    if ($atbDllSrc) {
        Write-Host ("     AntTweakBar.dll        (PE 0x{0:X4}) <- {1}" -f (Get-PeMachine -FilePath (Join-Path $GameDir "AntTweakBar.dll")), $atbDllSrc)
    }
}
Write-Host ("     shaders\*.hlsl/.hlsli  <- {0}" -f $shadersSrc)
if ($hasAsiLoader) {
    Write-Host ("     winmm.dll (ASI Loader) (PE 0x{0:X4})" -f $deployedWinmmMachine)
} else {
    Write-Warning "No winmm.dll ASI loader detected in $GameDir. Re-run with -InstallAsiLoader if no ASI loader is installed."
}
Write-Host "     Retail files verified untouched: $($retailBaselineHashes.Count) file(s)."
