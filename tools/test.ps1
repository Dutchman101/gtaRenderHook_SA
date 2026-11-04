param(
    [string]$Filter = "",
    [switch]$All
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$shaderDir = Join-Path $repoRoot "gtaRenderHook\shaders"
if (-not (Test-Path -LiteralPath $shaderDir -PathType Container)) {
    throw "DirectX 11 shader directory not found: $shaderDir"
}

$forbiddenPaths = @(
    "rh_engine_lib",
    "rw_rh_engine_lib",
    "rw_x64_render_driver",
    "gta_sa_render_hook",
    "gta_3_render_hook",
    "gta_vc_render_hook",
    "gtasa_render_hook_rt",
    "shaders\vulkan",
    "shaders\d3d11",
    "gtaRenderHook\RwVulkanEngine.h",
    "gtaRenderHook\RwVulkanEngine.cpp"
)
foreach ($rel in $forbiddenPaths) {
    $full = Join-Path $repoRoot $rel
    if (Test-Path -LiteralPath $full) {
        throw "Forbidden non-DX11 / legacy path still exists in repository: $rel"
    }
}
$leftoverVk = @(Get-ChildItem -LiteralPath (Join-Path $repoRoot "gtaRenderHook") -Filter "*Vulkan*" -File -ErrorAction SilentlyContinue)
if ($leftoverVk.Count -gt 0) {
    throw "Forbidden Vulkan files found in gtaRenderHook/: $($leftoverVk.Name -join ', ')"
}

$progFilesX86 = if (${env:ProgramFiles(x86)}) { ${env:ProgramFiles(x86)} } else { "C:\Program Files (x86)" }
$fxcCandidates = @(Get-ChildItem -Path (Join-Path $progFilesX86 "Windows Kits\10\bin\*\*\fxc.exe") -ErrorAction SilentlyContinue |
    Where-Object { $_.Directory.Name -in @("x64", "x86") } |
    Sort-Object `
        @{ Expression = { try { [version]$_.Directory.Parent.Name } catch { [version]"0.0" } }; Descending = $true }, `
        @{ Expression = { if ($_.Directory.Name -eq "x64") { 1 } else { 0 } }; Descending = $true })
if ($fxcCandidates.Count -gt 0) {
    $fxcPath = $fxcCandidates[0].FullName
} else {
    $fxcCmd = Get-Command fxc.exe -ErrorAction SilentlyContinue
    if (-not $fxcCmd) {
        throw "fxc.exe not found in Windows 10/11 SDK or PATH."
    }
    $fxcPath = $fxcCmd.Source
}

$shaderChecks = @(
    @{ File = "AmbientOcclusion.hlsl";        Entry = "AmbientOcclusionPS";        Stage = "ps" },
    @{ File = "AmbientOcclusion.hlsl";        Entry = "AODenoisePS";               Stage = "ps" },
    @{ File = "AtmosphericScattering.hlsl";   Entry = "AtmosphericScatteringPS";   Stage = "ps" },
    @{ File = "CloudRendering.hlsl";          Entry = "RenderCloudsPS";            Stage = "ps" },
    @{ File = "CloudRendering.hlsl";          Entry = "CloudsCombinePS";           Stage = "ps" },
    @{ File = "debug.hlsl";                   Entry = "VS";                        Stage = "vs" },
    @{ File = "debug.hlsl";                   Entry = "PS";                        Stage = "ps" },
    @{ File = "debug.hlsl";                   Entry = "RenderRasterPS";            Stage = "ps" },
    @{ File = "Deferred.hlsl";                Entry = "VS";                        Stage = "vs" },
    @{ File = "Deferred.hlsl";                Entry = "SunLightingPS";             Stage = "ps" },
    @{ File = "Deferred.hlsl";                Entry = "SunLightingPS";             Stage = "ps"; Defines = @("SAMPLE_SHADOWS=1", "BLUR_SHADOWS=0", "USE_PBR=1") },
    @{ File = "Deferred.hlsl";                Entry = "SunLightingPS";             Stage = "ps"; Defines = @("SAMPLE_SHADOWS=1", "BLUR_SHADOWS=1", "USE_PCS_SHADOWS=0", "USE_PBR=1") },
    @{ File = "Deferred.hlsl";                Entry = "SunLightingPS";             Stage = "ps"; Defines = @("SAMPLE_SHADOWS=1", "BLUR_SHADOWS=1", "USE_PCS_SHADOWS=1", "USE_PBR=1") },
    @{ File = "Deferred.hlsl";                Entry = "PointLightingPS";           Stage = "ps" },
    @{ File = "Deferred.hlsl";                Entry = "PointLightingPS";           Stage = "ps"; Defines = @("USE_PBR=1") },
    @{ File = "Deferred.hlsl";                Entry = "FinalPassPS";               Stage = "ps" },
    @{ File = "Deferred.hlsl";                Entry = "BlitPS";                    Stage = "ps" },
    @{ File = "HDRTonemapping.hlsl";          Entry = "LogLuminancePS";            Stage = "ps" },
    @{ File = "HDRTonemapping.hlsl";          Entry = "TonemapPS";                 Stage = "ps" },
    @{ File = "HDRTonemapping.hlsl";          Entry = "TonemapPS";                 Stage = "ps"; Defines = @("USE_GTA_CC=1") },
    @{ File = "HDRTonemapping.hlsl";          Entry = "DownScale2x2_LumPS";        Stage = "ps" },
    @{ File = "HDRTonemapping.hlsl";          Entry = "DownScale3x3PS";            Stage = "ps" },
    @{ File = "HDRTonemapping.hlsl";          Entry = "DownScale3x3_BrightPassPS"; Stage = "ps" },
    @{ File = "HDRTonemapping.hlsl";          Entry = "AdaptationPassPS";          Stage = "ps" },
    @{ File = "Quad.hlsl";                    Entry = "VS";                        Stage = "vs" },
    @{ File = "Quad.hlsl";                    Entry = "BlitPS";                    Stage = "ps" },
    @{ File = "RwIm2D.hlsl";                  Entry = "VS";                        Stage = "vs" },
    @{ File = "RwIm2D.hlsl";                  Entry = "PS";                        Stage = "ps" },
    @{ File = "RwIm3D.hlsl";                  Entry = "VS";                        Stage = "vs" },
    @{ File = "RwIm3D.hlsl";                  Entry = "PS";                        Stage = "ps" },
    @{ File = "RwIm3D.hlsl";                  Entry = "DeferredPS";                Stage = "ps" },
    @{ File = "RwMain.hlsl";                  Entry = "VS";                        Stage = "vs" },
    @{ File = "RwMain.hlsl";                  Entry = "PS";                        Stage = "ps" },
    @{ File = "RwMainTesselation.hlsl";       Entry = "VS";                        Stage = "vs" },
    @{ File = "RwMainTesselation.hlsl";       Entry = "HS";                        Stage = "hs"; MinModel = "5_0" },
    @{ File = "RwMainTesselation.hlsl";       Entry = "DS";                        Stage = "ds"; MinModel = "5_0" },
    @{ File = "RwMainTesselation.hlsl";       Entry = "PS";                        Stage = "ps" },
    @{ File = "RwSkin.hlsl";                  Entry = "VS";                        Stage = "vs" },
    @{ File = "RwSkin.hlsl";                  Entry = "PS";                        Stage = "ps" },
    @{ File = "RwSkin.hlsl";                  Entry = "DeferredPS";                Stage = "ps" },
    @{ File = "RwSkin.hlsl";                  Entry = "ShadowPS";                  Stage = "ps" },
    @{ File = "RwSkin.hlsl";                  Entry = "ShadowPS";                  Stage = "ps"; Defines = @("HAS_TEXTURE=1") },
    @{ File = "SACustomBuilding.hlsl";        Entry = "VS";                        Stage = "vs" },
    @{ File = "SACustomBuilding.hlsl";        Entry = "PS";                        Stage = "ps" },
    @{ File = "SACustomBuilding.hlsl";        Entry = "DeferredPS";                Stage = "ps" },
    @{ File = "SACustomBuilding.hlsl";        Entry = "ShadowPS";                  Stage = "ps" },
    @{ File = "SACustomBuilding.hlsl";        Entry = "ShadowPS";                  Stage = "ps"; Defines = @("HAS_TEXTURE=1") },
    @{ File = "SACustomBuilding.hlsl";        Entry = "VoxelVS";                   Stage = "vs"; MinModel = "5_0" },
    @{ File = "SACustomBuilding.hlsl";        Entry = "VoxelGS";                   Stage = "gs"; MinModel = "5_0" },
    @{ File = "SACustomBuilding.hlsl";        Entry = "VoxelPS";                   Stage = "ps"; MinModel = "5_0" },
    @{ File = "SACustomBuilding.hlsl";        Entry = "VoxelEmmissivePS";          Stage = "ps"; MinModel = "5_0" },
    @{ File = "SACustomBuildingDN.hlsl";      Entry = "VS";                        Stage = "vs" },
    @{ File = "SACustomBuildingDN.hlsl";      Entry = "PS";                        Stage = "ps" },
    @{ File = "SACustomBuildingDN.hlsl";      Entry = "DeferredPS";                Stage = "ps" },
    @{ File = "SACustomBuildingDN.hlsl";      Entry = "ShadowPS";                  Stage = "ps" },
    @{ File = "SACustomBuildingDN.hlsl";      Entry = "ShadowPS";                  Stage = "ps"; Defines = @("HAS_TEXTURE=1") },
    @{ File = "SACustomBuildingDN.hlsl";      Entry = "VoxelVS";                   Stage = "vs"; MinModel = "5_0" },
    @{ File = "SACustomBuildingDN.hlsl";      Entry = "VoxelGS";                   Stage = "gs"; MinModel = "5_0" },
    @{ File = "SACustomBuildingDN.hlsl";      Entry = "VoxelPS";                   Stage = "ps"; MinModel = "5_0" },
    @{ File = "SACustomBuildingDN.hlsl";      Entry = "VoxelEmmissivePS";          Stage = "ps"; MinModel = "5_0" },
    @{ File = "SACustomCarFX.hlsl";           Entry = "VS";                        Stage = "vs" },
    @{ File = "SACustomCarFX.hlsl";           Entry = "PS";                        Stage = "ps" },
    @{ File = "SACustomCarFX.hlsl";           Entry = "DeferredPS";                Stage = "ps" },
    @{ File = "SACustomCarFX.hlsl";           Entry = "ShadowPS";                  Stage = "ps" },
    @{ File = "SACustomCarFX.hlsl";           Entry = "ShadowPS";                  Stage = "ps"; Defines = @("HAS_TEXTURE=1") },
    @{ File = "SACustomCarFX.hlsl";           Entry = "VoxelVS";                   Stage = "vs"; MinModel = "5_0" },
    @{ File = "SACustomCarFX.hlsl";           Entry = "VoxelGS";                   Stage = "gs"; MinModel = "5_0" },
    @{ File = "SACustomCarFX.hlsl";           Entry = "VoxelPS";                   Stage = "ps"; MinModel = "5_0" },
    @{ File = "SACustomCarFX.hlsl";           Entry = "VoxelEmmissivePS";          Stage = "ps"; MinModel = "5_0" },
    @{ File = "SACustomSeabed.hlsl";          Entry = "VS";                        Stage = "vs" },
    @{ File = "SACustomSeabed.hlsl";          Entry = "PS";                        Stage = "ps" },
    @{ File = "SACustomSeabed.hlsl";          Entry = "DeferredPS";                Stage = "ps" },
    @{ File = "SACustomSeabed.hlsl";          Entry = "ShadowPS";                  Stage = "ps" },
    @{ File = "SACustomSeabed.hlsl";          Entry = "ShadowPS";                  Stage = "ps"; Defines = @("HAS_TEXTURE=1") },
    @{ File = "SACustomSeabed.hlsl";          Entry = "VoxelVS";                   Stage = "vs"; MinModel = "5_0" },
    @{ File = "SACustomSeabed.hlsl";          Entry = "VoxelGS";                   Stage = "gs"; MinModel = "5_0" },
    @{ File = "SACustomSeabed.hlsl";          Entry = "VoxelPS";                   Stage = "ps"; MinModel = "5_0" },
    @{ File = "SACustomSeabed.hlsl";          Entry = "VoxelEmmissivePS";          Stage = "ps"; MinModel = "5_0" },
    @{ File = "SACustomWater.hlsl";           Entry = "VS";                        Stage = "vs" },
    @{ File = "SACustomWater.hlsl";           Entry = "PS";                        Stage = "ps" },
    @{ File = "SACustomWater.hlsl";           Entry = "HS";                        Stage = "hs"; MinModel = "5_0" },
    @{ File = "SACustomWater.hlsl";           Entry = "DS";                        Stage = "ds"; MinModel = "5_0" },
    @{ File = "ScreenSpaceReflections.hlsl";  Entry = "ReflectionPassPS";          Stage = "ps" },
    @{ File = "ScreenSpaceReflections.hlsl";  Entry = "ReflectionPassPS";          Stage = "ps"; Defines = @("USE_SSR=1", "SAMPLE_CUBEMAP=1", "USE_PBR=1") },
    @{ File = "TemporalAA.hlsl";              Entry = "TAA_PS";                    Stage = "ps" },
    @{ File = "VolumetricLighting.hlsl";      Entry = "VolumetricSunlightPS";      Stage = "ps" },
    @{ File = "VolumetricLighting.hlsl";      Entry = "VolumetricCombinePS";       Stage = "ps" },
    @{ File = "RayTracing.hlsl";              Entry = "main";                      Stage = "cs"; MinModel = "5_0" },
    @{ File = "VoxelStuffCS.hlsl";            Entry = "FillVolume";                Stage = "cs"; MinModel = "5_0" }
)

$featureLevels = @(
    @{ Model = "5_0"; Define = "0xb000" }
)
if ($All) {
    $featureLevels += @(
        @{ Model = "4_1"; Define = "0xa100" },
        @{ Model = "4_0"; Define = "0xa000" }
    )
}

$passed = 0
$failed = 0

$candidateChecks = @(foreach ($fl in $featureLevels) {
    foreach ($check in $shaderChecks) {
        if ($check.MinModel -eq "5_0" -and $fl.Model -ne "5_0") {
            continue
        }
        $profile = "$($check.Stage)_$($fl.Model)"
        $defineSuffix = if ($check.Defines) { " [$($check.Defines -join ', ')]" } else { "" }
        [PSCustomObject]@{
            Check      = $check
            FlDefine   = $fl.Define
            Profile    = $profile
            CheckLabel = "$($check.File) [$($check.Entry) @ $profile]$defineSuffix"
        }
    }
})

if ($Filter) {
    $literalMatches = @($candidateChecks | Where-Object {
        $_.Check.File.IndexOf($Filter, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
        $_.Check.Entry.IndexOf($Filter, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
        $_.Profile.IndexOf($Filter, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or
        $_.CheckLabel.IndexOf($Filter, [System.StringComparison]::OrdinalIgnoreCase) -ge 0
    })
    if ($literalMatches.Count -gt 0) {
        $candidateChecks = $literalMatches
    } else {
        $filterRegex = try {
            [regex]::new($Filter, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        } catch {
            $null
        }
        $candidateChecks = if ($null -ne $filterRegex) {
            @($candidateChecks | Where-Object {
                $filterRegex.IsMatch($_.Check.File) -or
                $filterRegex.IsMatch($_.Check.Entry) -or
                $filterRegex.IsMatch($_.Profile) -or
                $filterRegex.IsMatch($_.CheckLabel)
            })
        } else {
            @()
        }
    }
}

foreach ($item in $candidateChecks) {
    $check = $item.Check
    $profile = $item.Profile
    $checkLabel = $item.CheckLabel
    $filePath = Join-Path $shaderDir $check.File
    if (-not (Test-Path -LiteralPath $filePath -PathType Leaf)) {
        Write-Host "[FAIL] Missing shader file: $filePath" -ForegroundColor Red
        $failed++
        continue
    }
    $fxcArgs = @("/nologo", "/T", $profile, "/E", $check.Entry, "/D", "FEATURE_LEVEL=$($item.FlDefine)")
    if ($check.Defines) {
        foreach ($def in $check.Defines) {
            $fxcArgs += @("/D", $def)
        }
    }
    $fxcArgs += @("/I", $shaderDir, $filePath)
    $output = & $fxcPath @fxcArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] $checkLabel" -ForegroundColor Red
        $output | ForEach-Object { Write-Host "       $_" }
        $failed++
    } else {
        $passed++
    }
}

if ($passed -eq 0 -and $failed -eq 0) {
    throw "No DirectX 11 shader checks matched -Filter '$Filter'."
}

Write-Host "[SUMMARY] DirectX 11 HLSL validation: $passed passed, $failed failed (fxc: $fxcPath)"
if ($failed -gt 0) {
    exit 1
}
