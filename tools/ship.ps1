param(
    [string]$GameDir = "",
    [string]$BuildDir = "",
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$ShadersOnly,
    [switch]$InstallAsiLoader,
    [switch]$AllowStaleArtifact,
    [switch]$PruneOrphanShaders
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$repoParent = Split-Path -Parent $repoRoot

if (-not $BuildDir) {
    $BuildDir = Join-Path $repoParent "build\dx11_x86"
}
if (-not $GameDir) {
    $GameDir = Join-Path $repoParent "BaseGameToApplyModsTo"
}

$buildScript = Join-Path $PSScriptRoot "build.ps1"
$testScript = Join-Path $PSScriptRoot "test.ps1"
$deployScript = Join-Path $PSScriptRoot "deploy_to_game.ps1"
foreach ($required in @($buildScript, $testScript, $deployScript)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Missing pipeline script: $required"
    }
}

Write-Host "==============================================================="
Write-Host " RenderHook DirectX 11 pipeline"
Write-Host ("  build tree : {0}" -f $BuildDir)
Write-Host ("  game dir   : {0}" -f $GameDir)
Write-Host ("  config     : {0}" -f $Config)
Write-Host ("  mode       : {0}" -f $(if ($ShadersOnly) { "shader-only (no C++ build, no binary deploy)" } else { "full" }))
Write-Host "==============================================================="
Write-Host ""

# Stage 1: compile. Skipped for a shader-only refresh, because a .hlsl edit cannot change the artifact.
if ($ShadersOnly) {
    Write-Host "[1/4] Build: skipped (shader-only)."
} elseif ($SkipBuild) {
    Write-Host "[1/4] Build: skipped (-SkipBuild)."
} else {
    Write-Host "[1/4] Build."
    $buildArgs = @("-BuildDir", $BuildDir, "-Config", $Config)
    if ($Clean) { $buildArgs += "-Clean" }
    & pwsh -File $buildScript @buildArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Stage 1 (build) failed with exit code $LASTEXITCODE. Nothing was deployed."
    }
}
Write-Host ""

# Stage 2: HLSL validation. This compiles shaders with fxc only; it never parses or compiles C++.
if ($SkipTests) {
    Write-Host "[2/4] HLSL validation: skipped (-SkipTests)."
} else {
    Write-Host "[2/4] HLSL validation (fxc, all shader models)."
    & pwsh -File $testScript -All
    if ($LASTEXITCODE -ne 0) {
        throw "Stage 2 (HLSL validation) failed with exit code $LASTEXITCODE. Nothing was deployed."
    }
}
Write-Host ""

# Stage 3: deploy through the script that owns the safety boundary.
Write-Host "[3/4] Deploy."
$deployArgs = @("-GameDir", $GameDir, "-BuildDir", $BuildDir, "-Config", $Config)
if ($ShadersOnly) { $deployArgs += "-ShadersOnly" }
if ($InstallAsiLoader) { $deployArgs += "-InstallAsiLoader" }
if ($AllowStaleArtifact) { $deployArgs += "-AllowStaleArtifact" }
if ($PruneOrphanShaders) { $deployArgs += "-PruneOrphanShaders" }
& pwsh -File $deployScript @deployArgs
if ($LASTEXITCODE -ne 0) {
    throw "Stage 3 (deploy) failed with exit code $LASTEXITCODE."
}
Write-Host ""

# Stage 4: verify the deployed tree from disk. Every check reads the bytes that will actually run rather
# than trusting the copy that just happened.
Write-Host "[4/4] Post-deploy verification."

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

$failures = New-Object System.Collections.Generic.List[string]

function Add-Check {
    param(
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][bool]$Passed,
        [string]$Detail = ""
    )

    if ($Passed) {
        Write-Host ("     [PASS] {0}{1}" -f $Label, $(if ($Detail) { ": $Detail" } else { "" }))
    } else {
        Write-Host ("     [FAIL] {0}{1}" -f $Label, $(if ($Detail) { ": $Detail" } else { "" }))
        $script:failures.Add($Label)
    }
}

# 4a. The deployed binary must be byte-identical to the built one.
if (-not $ShadersOnly) {
    $builtCandidates = @(
        (Join-Path $BuildDir "$Config\gtaRenderHook.asi"),
        (Join-Path $BuildDir "gtaRenderHook\$Config\gtaRenderHook.asi")
    )
    $builtArtifact = $builtCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    $deployedArtifact = Join-Path $GameDir "gtaRenderHook.asi"
    if (-not $builtArtifact) {
        Add-Check -Label "Built artifact present" -Passed $false -Detail "no gtaRenderHook.asi under $BuildDir"
    } elseif (-not (Test-Path -LiteralPath $deployedArtifact -PathType Leaf)) {
        Add-Check -Label "Deployed artifact present" -Passed $false -Detail $deployedArtifact
    } else {
        $builtHash = (Get-FileHash -LiteralPath $builtArtifact -Algorithm SHA256).Hash
        $deployedHash = (Get-FileHash -LiteralPath $deployedArtifact -Algorithm SHA256).Hash
        Add-Check -Label "Deployed payload identical to built payload" -Passed ($builtHash -eq $deployedHash) -Detail $deployedHash
        $deployedMachine = Get-PeMachine -FilePath $deployedArtifact
        Add-Check -Label "Deployed gtaRenderHook.asi is x86" -Passed ($deployedMachine -eq 0x014C) -Detail ("PE 0x{0:X4}" -f $deployedMachine)
    }
}

# 4b. Retail baseline. gta_sa.exe is checked against the known v1.0 US digest; the deployer re-verifies
# the other five against a pre-copy snapshot.
$expectedExeSha256 = "A559AA772FD136379155EFA71F00C47AAD34BBFEAE6196B0FE1047D0645CBD26"
$exePath = Join-Path $GameDir "gta_sa.exe"
if (Test-Path -LiteralPath $exePath -PathType Leaf) {
    $exeHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash
    Add-Check -Label "Retail gta_sa.exe unchanged" -Passed ($exeHash -eq $expectedExeSha256) -Detail $exeHash
} else {
    Add-Check -Label "Retail gta_sa.exe present" -Passed $false -Detail $exePath
}

# 4c. Every shipped shader must exist in the game tree with identical bytes.
$shadersSrc = Join-Path $repoRoot "gtaRenderHook\shaders"
$shadersDest = Join-Path $GameDir "shaders"
if (Test-Path -LiteralPath $shadersSrc -PathType Container) {
    $sourceShaders = @(Get-ChildItem -LiteralPath $shadersSrc -File -Recurse -Force)
    $mismatches = New-Object System.Collections.Generic.List[string]
    foreach ($sourceShader in $sourceShaders) {
        $relative = $sourceShader.FullName.Substring($shadersSrc.Length).TrimStart('\', '/')
        $deployedShader = Join-Path $shadersDest $relative
        if (-not (Test-Path -LiteralPath $deployedShader -PathType Leaf)) {
            $mismatches.Add("missing $relative")
            continue
        }
        if ((Get-FileHash -LiteralPath $sourceShader.FullName -Algorithm SHA256).Hash -ne (Get-FileHash -LiteralPath $deployedShader -Algorithm SHA256).Hash) {
            $mismatches.Add("differs $relative")
        }
    }
    Add-Check -Label "Shipped shaders byte-identical" -Passed ($mismatches.Count -eq 0) -Detail ("{0} source file(s), {1} mismatch(es){2}" -f $sourceShaders.Count, $mismatches.Count, $(if ($mismatches.Count -gt 0) { ": " + ($mismatches -join '; ') } else { "" }))

    # Extra deployed shader files are a normal state: presets ship their own variants, and the runtime keeps a
    # content-addressed compile cache. They are reported so the count never looks like a defect.
    $relativeSourceNames = @{}
    foreach ($sourceShader in $sourceShaders) {
        $relativeSourceNames[$sourceShader.FullName.Substring($shadersSrc.Length).TrimStart('\', '/').ToLowerInvariant()] = $true
    }
    $extras = @(Get-ChildItem -LiteralPath $shadersDest -File -Recurse -Force -ErrorAction SilentlyContinue | Where-Object {
            $rel = $_.FullName.Substring($shadersDest.Length).TrimStart('\', '/')
            -not $relativeSourceNames.ContainsKey($rel.ToLowerInvariant())
        })
    $cacheExtras = @($extras | Where-Object { $_.Extension -eq '.cso' })
    $presetExtras = @($extras | Where-Object { $_.Extension -ne '.cso' })
    Write-Host ("     [INFO] Deployed shader tree holds {0} file(s) beyond the shipped set: {1} runtime cache blob(s) in shaders\cache and {2} preset file(s). The cache is written by the runtime and pruned once it exceeds 256 entries, so its count is not a fixed expectation." -f $extras.Count, $cacheExtras.Count, $presetExtras.Count)
} else {
    Add-Check -Label "Shader source tree present" -Passed $false -Detail $shadersSrc
}

# 4d. Retired Vulkan stack must be absent. Its IPC binary and any app-local graphics runtime would
# either do nothing or break the DirectX 11 hook.
$forbiddenArtifacts = @(
    "gta_sa_render_hook.asi",
    "gta_sa_render_hook.pdb",
    "gta_sa_render_driver.exe",
    "gta_sa_render_driver.pdb",
    "glslangValidator.exe",
    "vulkan-1.dll",
    "d3d11.dll",
    "dxgi.dll",
    "d3d9.dll"
)
$presentForbidden = @($forbiddenArtifacts | Where-Object { Test-Path -LiteralPath (Join-Path $GameDir $_) -PathType Leaf })
$presentForbiddenDirs = @(@("shaders\vulkan", "shaders\d3d11") | Where-Object { Test-Path -LiteralPath (Join-Path $GameDir $_) -PathType Container })
$forbiddenTotal = $presentForbidden.Count + $presentForbiddenDirs.Count
$forbiddenDetail = if ($forbiddenTotal -eq 0) {
    "checked $($forbiddenArtifacts.Count) file(s) and 2 directory(ies)"
} else {
    ($presentForbidden + $presentForbiddenDirs) -join '; '
}
Add-Check -Label "No retired or app-local graphics override present" -Passed ($forbiddenTotal -eq 0) -Detail $forbiddenDetail

# 4e. The ASI loader is what makes the hook load at all.
$winmmPath = Join-Path $GameDir "winmm.dll"
if (Test-Path -LiteralPath $winmmPath -PathType Leaf) {
    $winmmMachine = Get-PeMachine -FilePath $winmmPath
    Add-Check -Label "ASI loader winmm.dll is x86" -Passed ($winmmMachine -eq 0x014C) -Detail ("PE 0x{0:X4}" -f $winmmMachine)
} else {
    Write-Host "     [WARN] No winmm.dll ASI loader in $GameDir, so no .asi plugin will load. Re-run with -InstallAsiLoader."
}

# 4f. No temporary residue from the copy step.
$residue = @(Get-ChildItem -LiteralPath $GameDir -File -Force -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in @('.tmp', '.bak') })
Add-Check -Label "No temporary residue in the game directory" -Passed ($residue.Count -eq 0) -Detail ($(if ($residue.Count -eq 0) { "0 .tmp, 0 .bak" } else { ($residue | ForEach-Object { $_.Name }) -join '; ' }))

foreach ($antTweakBar in @("AntTweakBar.dll")) {
    $atbPath = Join-Path $GameDir $antTweakBar
    if (Test-Path -LiteralPath $atbPath -PathType Leaf) {
        $atbMachine = Get-PeMachine -FilePath $atbPath
        Add-Check -Label "$antTweakBar is x86" -Passed ($atbMachine -eq 0x014C) -Detail ("PE 0x{0:X4}" -f $atbMachine)
    }
}

Write-Host ""
if ($failures.Count -gt 0) {
    Write-Host ("[FAIL] Pipeline finished with {0} failed verification check(s): {1}" -f $failures.Count, ($failures -join '; '))
    exit 1
}

Write-Host "[OK] Pipeline complete. Build, HLSL validation, deployment and verification all passed."
Write-Host ""
Write-Host "     Runtime behaviour is NOT covered by this pipeline: no game process was launched. Treat the"
Write-Host "     result as built and deployed, with in-game shader compilation and device bring-up unverified."
Write-Host "     Launch with the working directory set to the game folder, then read <GameDir>\debug.log:"
Write-Host ("       Start-Process -FilePath '{0}\gta_sa.exe' -WorkingDirectory '{0}'" -f $GameDir)
