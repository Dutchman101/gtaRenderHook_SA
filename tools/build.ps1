param(
    [string]$BuildDir = "",
    [string]$Config = "Release",
    [switch]$Clean,
    [string]$GTAFolder = "",
    [string]$Generator = "Visual Studio 18 2026"
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$repoParent = Split-Path -Parent $repoRoot

if (-not $BuildDir) {
    $BuildDir = Join-Path $repoParent "build\dx11_x86"
}
if (-not $GTAFolder) {
    $defaultGame = Join-Path $repoParent "BaseGameToApplyModsTo"
    if (Test-Path -LiteralPath $defaultGame -PathType Container) {
        $GTAFolder = $defaultGame
    }
}

function Assert-SafeBuildDir {
    param([Parameter(Mandatory = $true)][string]$Path)

    $full = [System.IO.Path]::GetFullPath($Path)
    $parentFull = [System.IO.Path]::GetFullPath($repoParent)
    $leaf = Split-Path -Leaf $full
    if (-not $leaf) {
        throw "Refusing to operate on a build directory with no leaf name: $full"
    }
    $under = $full.StartsWith($parentFull + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)
    if (-not $under) {
        throw "Refusing to build outside the workspace parent ($parentFull): $full"
    }
    if ($full.TrimEnd('\', '/') -eq $parentFull.TrimEnd('\', '/') -or $full.TrimEnd('\', '/') -eq [System.IO.Path]::GetFullPath($repoRoot).TrimEnd('\', '/')) {
        throw "Refusing to use the workspace root or the repository root as a build directory: $full"
    }
    return $full
}

$BuildDir = Assert-SafeBuildDir -Path $BuildDir

foreach ($tool in @("cmake")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required tool '$tool' is not on PATH."
    }
}

# Named so a stale-tree failure is self-explanatory rather than surfacing as a missing artifact later.
Write-Host "[1/3] Build tree: $BuildDir"

if ($Clean) {
    if (Test-Path -LiteralPath $BuildDir -PathType Container) {
        $staleObjects = @(Get-ChildItem -LiteralPath $BuildDir -Filter "*.obj" -Recurse -File -Force -ErrorAction SilentlyContinue)
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
        Write-Host "     Full clean: removed the tree and $($staleObjects.Count) object file(s)."
    } else {
        Write-Host "     Full clean: the tree did not exist, so nothing was removed."
    }
}

$cacheFile = Join-Path $BuildDir "CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cacheFile -PathType Leaf)) {
    $configureArgs = @("-S", $repoRoot, "-B", $BuildDir, "-G", $Generator, "-A", "Win32")
    if ($GTAFolder) {
        # GTA_SA_FOLDER feeds install() rules only, so it never redirects the build output. It is set for
        # cache parity with the documented configure line and nothing else.
        $configureArgs += "-DGTA_SA_FOLDER=$($GTAFolder -replace '\\', '/')"
    }
    Write-Host "[2/3] Configure: absent cache, configuring a fresh Win32 tree."
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) {
        throw "cmake configure failed with exit code $LASTEXITCODE."
    }
} else {
    $cachedPlatform = (Select-String -LiteralPath $cacheFile -Pattern '^CMAKE_GENERATOR_PLATFORM:' -SimpleMatch:$false -ErrorAction SilentlyContinue |
        Select-Object -First 1).Line
    if ($cachedPlatform -and ($cachedPlatform -notmatch '(?i)win32')) {
        throw "Existing tree at $BuildDir is not a Win32 tree ($cachedPlatform). Re-run with -Clean to reconfigure it as Win32, which is mandatory for a 32-bit gta_sa.exe."
    }
    Write-Host "[2/3] Configure: reusing the existing Win32 tree."
}

Write-Host "[3/3] Build: target gtaRenderHook ($Config)."
$buildLog = New-Object System.Collections.Generic.List[string]
& cmake --build $BuildDir --config $Config --target gtaRenderHook 2>&1 | ForEach-Object {
    $line = if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { [string]$_ }
    $buildLog.Add($line)
    Write-Host $line
}
$buildExit = $LASTEXITCODE

$warningCount = @($buildLog | Where-Object { $_ -match '(?i)\bwarning\s+[A-Z]+\d+' }).Count
$errorCount = @($buildLog | Where-Object { $_ -match '(?i)\berror\s+[A-Z]+\d+' }).Count

if ($buildExit -ne 0) {
    Write-Host ""
    Write-Host ("[FAIL] Build exited {0} with {1} error line(s) and {2} warning line(s)." -f $buildExit, $errorCount, $warningCount)
    exit $buildExit
}

# The Visual Studio generator places the artifact either directly in the configuration directory or beside
# the target directory, so both shapes are checked rather than one assumed.
$artifactCandidates = @(
    (Join-Path $BuildDir "$Config\gtaRenderHook.asi"),
    (Join-Path $BuildDir "gtaRenderHook\$Config\gtaRenderHook.asi")
)
$artifact = $artifactCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
if (-not $artifact) {
    throw "Build reported success but produced no gtaRenderHook.asi. Checked: $($artifactCandidates -join '; ')"
}

function Get-PeMachine {
    param([Parameter(Mandatory = $true)][string]$FilePath)

    $stream = [System.IO.File]::Open($FilePath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        $dosHeader = New-Object byte[] 64
        if ($stream.Read($dosHeader, 0, 64) -lt 64 -or $dosHeader[0] -ne 0x4D -or $dosHeader[1] -ne 0x5A) {
            throw "Not a valid PE binary (missing MZ header): $FilePath"
        }
        $peOffset = [System.BitConverter]::ToInt32($dosHeader, 0x3C)
        [void]$stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin)
        $peHeader = New-Object byte[] 6
        if ($stream.Read($peHeader, 0, 6) -lt 6 -or ([System.BitConverter]::ToUInt32($peHeader, 0) -ne 0x00004550)) {
            throw "Missing PE signature in: $FilePath"
        }
        return [System.BitConverter]::ToUInt16($peHeader, 4)
    } finally {
        $stream.Dispose()
    }
}

$item = Get-Item -LiteralPath $artifact
$machine = Get-PeMachine -FilePath $artifact
if ($machine -ne 0x014C) {
    throw ("gtaRenderHook.asi must be 32-bit x86 (0x014C), got 0x{0:X4}. Re-run with -Clean so a stale Win32-only cache cannot be reused." -f $machine)
}

$objectCount = @(Get-ChildItem -LiteralPath $BuildDir -Filter "*.obj" -Recurse -File -Force -ErrorAction SilentlyContinue).Count
$hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash

Write-Host ""
Write-Host "[OK] Build succeeded."
Write-Host ("     Artifact      : {0}" -f $artifact)
Write-Host ("     Size          : {0:N0} bytes" -f $item.Length)
Write-Host ("     PE machine    : 0x{0:X4} (x86)" -f $machine)
Write-Host ("     Modified (UTC): {0:o}" -f $item.LastWriteTimeUtc)
Write-Host ("     SHA-256       : {0}" -f $hash)
Write-Host ("     Object files  : {0}" -f $objectCount)
Write-Host ("     Diagnostics   : {0} warning line(s), {1} error line(s)" -f $warningCount, $errorCount)
Write-Host ""
Write-Host "     This SHA-256 changes on every build at an identical size, because the linker stamps a"
Write-Host "     timestamp and a fresh PDB GUID into the image. Never compare it against a hash recorded"
Write-Host "     from an earlier build to decide whether sources changed."
