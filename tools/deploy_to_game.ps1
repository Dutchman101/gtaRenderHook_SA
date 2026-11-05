param(
    [string]$GameDir = "",
    [string]$BuildDir = "",
    [string]$Config = "Release",
    [switch]$InstallAsiLoader,
    [string]$AsiLoaderPath = "",
    [switch]$ShadersOnly,
    [switch]$SkipExeHashCheck,
    [switch]$AllowStaleArtifact,
    [switch]$PruneOrphanShaders
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$repoParent = Split-Path -Parent $repoRoot

if (-not $GameDir) {
    $GameDir = Join-Path $repoParent "BaseGameToApplyModsTo"
}
if (-not $BuildDir) {
    $BuildDir = Join-Path $repoParent "build\dx11_x86"
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

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Get-Item -LiteralPath $candidate).FullName
        }
    }
    throw "Required artifact '$Label' not found in the build tree '$BuildDir'. Checked: $($Candidates -join '; '). Build it with tools\build.ps1, or pass -BuildDir pointing at the tree that holds it."
}

function Assert-ArtifactFresh {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$ArtifactPath,
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [string[]]$ExtraFiles = @(),
        [switch]$Override
    )

    if (-not (Test-Path -LiteralPath $SourceRoot -PathType Container)) {
        throw "Source root for freshness check does not exist: $SourceRoot"
    }
    $artifact = Get-Item -LiteralPath $ArtifactPath
    # The shaders tree is excluded as a whole because it ships as runtime data that the runtime compiles, so an
    # edit there cannot change the compiled artifact and must not block a deployment. Every other compilation
    # input under the source root is scanned, include fragments too, because an edit there does change it.
    # Dropping this exclusion makes a shader edit block a deployment; narrowing the extension list lets a
    # fragment edit pass unseen.
    $sourceExtensions = @('.cpp', '.cc', '.c', '.h', '.hpp', '.inl', '.inc')
    $shaderDataPrefix = (Join-Path $SourceRoot 'shaders').TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $sources = @(Get-ChildItem -LiteralPath $SourceRoot -File -Recurse -Force |
        Where-Object { -not $_.FullName.StartsWith($shaderDataPrefix, [System.StringComparison]::OrdinalIgnoreCase) } |
        Where-Object { $sourceExtensions -contains $_.Extension -or $_.Name -eq 'CMakeLists.txt' })
    foreach ($extra in $ExtraFiles) {
        if ($extra -and (Test-Path -LiteralPath $extra -PathType Leaf)) {
            $sources += Get-Item -LiteralPath $extra
        }
    }
    $newer = @($sources | Where-Object { $_.LastWriteTimeUtc -gt $artifact.LastWriteTimeUtc } |
        Sort-Object LastWriteTimeUtc -Descending)
    if ($newer.Count -eq 0) {
        return ([PSCustomObject]@{ SourceCount = $sources.Count; NewerCount = 0; Overridden = $false })
    }
    $preview = ($newer | Select-Object -First 10 | ForEach-Object { "{0} ({1:o})" -f $_.FullName, $_.LastWriteTimeUtc }) -join '; '
    $message = "Artifact '$Label' is older than $($newer.Count) source file(s) and does not contain them: $preview. Rebuild before deploying, or pass -AllowStaleArtifact if the newer edits cannot affect this artifact (for example comment-only changes)."
    if ($Override) {
        Write-Warning $message
        return ([PSCustomObject]@{ SourceCount = $sources.Count; NewerCount = $newer.Count; Overridden = $true })
    }
    throw $message
}

function Copy-VerifiedFile {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath
    )

    $parent = Split-Path -Parent $DestinationPath
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $parent | Out-Null
    }
    Copy-Item -LiteralPath $SourcePath -Destination $DestinationPath -Force
    $sourceHash = (Get-FileHash -LiteralPath $SourcePath -Algorithm SHA256).Hash
    $destHash = (Get-FileHash -LiteralPath $DestinationPath -Algorithm SHA256).Hash
    if ($sourceHash -ne $destHash) {
        throw "Post-copy verification failed: $DestinationPath (expected $sourceHash, got $destHash)"
    }
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

$hookSrc = $null
$hookPdbSrc = $null
$atbDllSrc = $null
# Filled in only when the hook artifact passed the freshness gate; the closing report prints it as evidence.
$freshness = $null
if (-not $ShadersOnly) {
    $hookSrc = Resolve-ExistingFile -Label "gtaRenderHook.asi (x86 DirectX 11)" -Candidates @(
        (Join-Path $BuildDir "$Config\gtaRenderHook.asi"),
        (Join-Path $BuildDir "gtaRenderHook\$Config\gtaRenderHook.asi"),
        (Join-Path $BuildDir "gtaRenderHook.asi"),
        (Join-Path $BuildDir "gtaRenderHook\gtaRenderHook.asi")
    )
    $freshness = Assert-ArtifactFresh -Label "gtaRenderHook.asi" -ArtifactPath $hookSrc -SourceRoot (Join-Path $repoRoot "gtaRenderHook") -ExtraFiles @((Join-Path $repoRoot "CMakeLists.txt")) -Override:$AllowStaleArtifact
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
    $dlDir = Split-Path -Parent $AsiLoaderPath
    $shaPath = Join-Path $dlDir "winmm-Win32.SHA512"
    if (-not (Test-Path -LiteralPath $AsiLoaderPath -PathType Leaf)) {
        New-Item -ItemType Directory -Force -Path $dlDir | Out-Null
        $zipPath = Join-Path $dlDir "Ultimate-ASI-Loader_Win32.zip"
        $baseUrl = "https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/Win32-latest"
        Invoke-WebRequest -Uri "$baseUrl/Ultimate-ASI-Loader_Win32.zip" -OutFile $zipPath
        Invoke-WebRequest -Uri "$baseUrl/winmm-Win32.SHA512" -OutFile $shaPath
        Expand-Archive -LiteralPath $zipPath -DestinationPath $dlDir -Force
    }
    # The cached loader is checked against the publisher's digest even when it was already on disk, so an existing
    # file is not trusted on its presence alone. The digest is picked out by its shape because the published sidecar
    # is a "Hash : <128 hex>" record rather than a bare digest, and a sidecar holding no single digest is rejected.
    if (Test-Path -LiteralPath $shaPath -PathType Leaf) {
        $shaDigests = @((Get-Content -LiteralPath $shaPath -Raw) -split '\s+' | Where-Object { $_ -match '^[0-9A-Fa-f]{128}$' })
        if ($shaDigests.Count -ne 1) {
            throw "Expected exactly one SHA-512 digest in $shaPath, found $($shaDigests.Count); the ASI loader cannot be accepted without a digest to check it against."
        }
        $expectedSha512 = ([string]$shaDigests[0]).ToUpperInvariant()
        $actualSha512 = (Get-FileHash -LiteralPath $AsiLoaderPath -Algorithm SHA512).Hash.ToUpperInvariant()
        if ($actualSha512 -ne $expectedSha512) {
            Remove-Item -LiteralPath $AsiLoaderPath -Force -ErrorAction SilentlyContinue
            throw "Ultimate ASI Loader SHA-512 mismatch: expected $expectedSha512, got $actualSha512"
        }
    } else {
        Write-Warning "No published digest at $shaPath, so the cached ASI loader was not checked against the publisher's hash. Delete it to force a fresh download with a digest to check against."
    }
    $ualMachine = Get-PeMachine -FilePath $AsiLoaderPath
    if ($ualMachine -ne 0x014C) {
        throw ("Ultimate ASI Loader winmm.dll must be 32-bit x86 (0x014C), got 0x{0:X4}" -f $ualMachine)
    }
    Copy-VerifiedFile -SourcePath $AsiLoaderPath -DestinationPath (Join-Path $GameDir "winmm.dll")
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
    Copy-VerifiedFile -SourcePath $hookSrc -DestinationPath (Join-Path $GameDir "gtaRenderHook.asi")
    $destPdb = Join-Path $GameDir "gtaRenderHook.pdb"
    if ($hookPdbSrc) {
        Copy-VerifiedFile -SourcePath $hookPdbSrc -DestinationPath $destPdb
    } elseif (Test-Path -LiteralPath $destPdb -PathType Leaf) {
        Remove-Item -LiteralPath $destPdb -Force
        Write-Host "     Removed stale symbol file: gtaRenderHook.pdb"
    }
    if ($atbDllSrc) {
        Copy-VerifiedFile -SourcePath $atbDllSrc -DestinationPath (Join-Path $GameDir "AntTweakBar.dll")
    }
}

$destShaders = Join-Path $GameDir "shaders"
New-Item -ItemType Directory -Force -Path $destShaders | Out-Null

# The whole shaders tree is shipped rather than a list of shader extensions, because a shader may include a fragment
# of any extension and a deployed tree missing one fails only when the runtime compiles it. The orphan scan below
# stays limited to shader extensions so -PruneOrphanShaders can never remove a preset's own data files.
$shaderFiles = @(Get-ChildItem -LiteralPath $shadersSrc -File -Recurse -Force)
foreach ($shaderFile in $shaderFiles) {
    $relative = $shaderFile.FullName.Substring($shadersSrc.Length).TrimStart('\', '/')
    Copy-VerifiedFile -SourcePath $shaderFile.FullName -DestinationPath (Join-Path $destShaders $relative)
}

# The game tree may legitimately hold shader files this tool never deployed, for example a preset that ships its
# own variant of an existing shader. Orphans are therefore reported by name and removed only on explicit request,
# never pruned implicitly.
$deployedShaderNames = @{}
foreach ($shaderFile in $shaderFiles) {
    $deployedShaderNames[$shaderFile.FullName.Substring($shadersSrc.Length).TrimStart('\', '/').ToLowerInvariant()] = $true
}
$orphanShaders = @(Get-ChildItem -LiteralPath $destShaders -File -Recurse -Force |
    Where-Object { $_.Extension -in @('.hlsl', '.hlsli') } |
    Where-Object { -not $deployedShaderNames.ContainsKey($_.FullName.Substring($destShaders.Length).TrimStart('\', '/').ToLowerInvariant()) })
if ($orphanShaders.Count -gt 0) {
    $orphanList = ($orphanShaders | ForEach-Object { $_.FullName.Substring($destShaders.Length).TrimStart('\', '/') }) -join '; '
    if ($PruneOrphanShaders) {
        foreach ($orphan in $orphanShaders) {
            Remove-Item -LiteralPath $orphan.FullName -Force
            Write-Host ("     Removed orphan shader: {0}" -f $orphan.FullName.Substring($destShaders.Length).TrimStart('\', '/'))
        }
    } else {
        Write-Warning "Deployed shader tree contains $($orphanShaders.Count) file(s) with no source counterpart: $orphanList. These are left in place because presets may ship their own shaders. Pass -PruneOrphanShaders to remove them."
    }
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

# Both deployed binaries are read back before the success line, so nothing can fail after the report has already
# called the deployment complete.
$deployedHookMachine = $null
$deployedAtbMachine = $null
if (-not $ShadersOnly) {
    $deployedHookMachine = Get-PeMachine -FilePath (Join-Path $GameDir "gtaRenderHook.asi")
    if ($atbDllSrc) {
        $deployedAtbMachine = Get-PeMachine -FilePath (Join-Path $GameDir "AntTweakBar.dll")
    }
}

Write-Host "[OK] Deployed RenderHook DirectX 11 package to: $GameDir"
if ($freshness) {
    if ($freshness.Overridden) {
        Write-Host ("     Freshness: artifact is older than {0} source file(s); deployment was forced with -AllowStaleArtifact." -f $freshness.NewerCount)
    } else {
        Write-Host ("     Freshness: artifact postdates all {0} source file(s)." -f $freshness.SourceCount)
    }
}
if (-not $ShadersOnly) {
    Write-Host ("     gtaRenderHook.asi      (PE 0x{0:X4}) <- {1}" -f $deployedHookMachine, $hookSrc)
    if ($hookPdbSrc) {
        Write-Host ("     gtaRenderHook.pdb                   <- {0}" -f $hookPdbSrc)
    }
    if ($atbDllSrc) {
        Write-Host ("     AntTweakBar.dll        (PE 0x{0:X4}) <- {1}" -f $deployedAtbMachine, $atbDllSrc)
    }
}
Write-Host ("     shaders\*.hlsl/.hlsli  <- {0}" -f $shadersSrc)
if ($hasAsiLoader) {
    Write-Host ("     winmm.dll (ASI Loader) (PE 0x{0:X4})" -f $deployedWinmmMachine)
} else {
    Write-Warning "No winmm.dll ASI loader detected in $GameDir. Re-run with -InstallAsiLoader if no ASI loader is installed."
}
Write-Host "     Retail files verified untouched: $($retailBaselineHashes.Count) file(s)."
