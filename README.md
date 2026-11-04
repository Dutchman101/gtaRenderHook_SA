# GTA San Andreas RenderHook (DirectX 11)

DirectX 11 rendering hook for **Grand Theft Auto: San Andreas (`v1.0 US`)**.

This repository exclusively targets GTA San Andreas (`v1.0 US`) with an in-process **DirectX 11** (`gtaRenderHook.asi`) rendering backend (`gtaRenderHook/`).

## Features

* **Deferred Rendering & PBR Shading:** Multi-target G-buffer, physically based lighting, and material system (`gtaRenderHook/shaders/Deferred.hlsl`, `GBuffer.hlsl`, `LightingFunctions.hlsl`).
* **Cascaded Shadow Maps (CSM):** 4-cascade sphere-fitted directional shadows with texel-snapped light-space projection, camera-cut detection, and soft PCSS filtering (`ShadowRenderer.cpp`, `gtaRenderHook/shaders/Shadows.hlsl`).
* **Ambient Occlusion (GTAO) & Screen-Space Reflections (SSR):** Ground-truth ambient occlusion with spatial denoising (`AmbientOcclusion.cpp`, `AmbientOcclusion.hlsl`) and hierarchical screen-space reflections (`ScreenSpaceReflections.hlsl`, `ReflectionFunctions.hlsli`).
* **Atmospheric Scattering, Volumetric Lighting & Volumetric Clouds:** Rayleigh/Mie sky scattering, godrays, and ray-marched clouds (`AtmosphericScattering.hlsl`, `VolumetricLighting.hlsl`, `CloudRendering.hlsl`).
* **Temporal Anti-Aliasing (TAA) & External Plugin API:** Per-object and skinned motion vectors, reversed-Z depth buffer, and `RenderHookAPI.h` callbacks (`TemporalAA.cpp`, `TemporalAA.hlsl`).
* **Custom GTA SA Pipelines:** Dedicated DirectX 11 pipelines for buildings (`SACustomBuilding.hlsl`, `SACustomBuildingDN.hlsl` with dual-pass alpha foliage and dithered LOD distance fading), vehicles (`SACustomCarFX.hlsl`), skinned peds (`RwSkin.hlsl`), water (`SACustomWater.hlsl`), and seabed (`SACustomSeabed.hlsl`).

## Requirements

1. **Visual Studio C++ Toolchain** (MSVC `v142` / `v143` / `v145` with C++17 and 32-bit `Win32` x86 support)
2. **Windows 10/11 SDK** (providing DirectX 11 headers/libs and `fxc.exe`)
3. **External Libraries** (configured via `gtaRenderHook/DependencyPaths.props` or environment variables):
   * `plugin-sdk` (`GTA_SA;GTASA;PLUGIN_SGV_10US`)
   * `AntTweakBar`
   * `tinyxml2`
4. **GTA San Andreas `v1.0 US`** (`gta_sa.exe`) + [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (`winmm.dll`, Win32)

## Build, Shader Validation & Deployment

1. **Build `gtaRenderHook.asi` (Win32 / x86):**
   Open `gtaRenderHook.sln` (`gtaRenderHook/gtaRenderHook.vcxproj`) in Visual Studio or build via MSBuild (`Release|x86` / `Debug|x86`).
2. **Validate all DirectX 11 HLSL shaders (`fxc.exe` across Shader Models `5_0`, `4_1`, and `4_0`):**
   ```powershell
   pwsh ./tools/test.ps1 -All
   ```
3. **Deploy to a GTA San Andreas `v1.0 US` installation:**
   ```powershell
   # Initial deploy (Ultimate ASI Loader winmm.dll + DirectX 11 gtaRenderHook.asi + shaders):
   pwsh ./tools/deploy_to_game.ps1 -GameDir "<path_to_gta_sa>" -InstallAsiLoader

   # Subsequent code/shader iterations:
   pwsh ./tools/deploy_to_game.ps1 -GameDir "<path_to_gta_sa>"
   ```

## Reporting Issues

If you encounter a bug, please attach `debug.log`, `settings.xml`, crash dumps, and your GPU/OS details to your report.
