# GTA rendering hook

GTA rendering hook

Implements DirectX11 and Vulkan rendering API for GTA San Andreas (GTA SA).


Currently there are 2 versions, one of which is broken(can't really build it anymore):

* Legacy in-process DirectX 11 plugin (`gtaRenderHook/gtaRenderHook.vcxproj`, unmaintained).
* Active out-of-process Vulkan 1.2 + hardware ray tracing stack (`VK_KHR_acceleration_structure` / `VK_KHR_ray_tracing_pipeline`, supporting Vulkan 1.2 ray-tracing GPUs) with ReSTIR direct lighting and shadows, ray-traced primary G-buffer, ambient occlusion, and reflections.

We have [discord server](https://discord.gg/rsZEUNW), come and help to improve it!

## How it works?

TL;DR: Currently we have incomplete RW rendering backend implementation that communicates via shared memory to
incomplete rendering engine instance running rendering loop based on Vulkan API.

Long version:

* Game rendering engine code is partially redirected to shared memory task queue
* Task queue loads game assets to GPU memory, processes various requests like retrieving supported resolutions etc.
* One of the tasks prepares a frame to framebuffer based on uploaded scene data, each frame draw calls is uploaded to
  64bit process via task queue.
* Separation between rendering and game code is done for several reasons:
    * It allows to use newer drivers, and support raytracing extensions
    * It allows to use more memory if needed
    * Surprisingly it works faster!
* Rendering right now is basic, and very hacky:
    * BLAS is built for each model instance being loaded by the game.
    * Animations are prepared by compute shaders and rebuilt every frame
    * TLAS is rebuilt every frame
    * Scene is loaded to GPU, contains all materials, textures, vertex/index buffers and transform data
    * Lights are prepared using very basic tiled culling implementation
    * Primary rays are traced to compute G-Buffers: Albedo, Normals, LinearDepth, Motion vectors and Material params
    * AO rays are traced, temporally accumulated and blurred via bilateral blur with varying kernel(based on temporal
      variance)
    * Shading for sunlight and each point light inside a tile is generated, then temporally accumulated and blurred via
      bilateral blur(based on temporal luminance variance)
    * Reflections are traced with one shadow ray per-pixel and blurred via same filter that is used for shading
    * Everything is combined based on material properties etc.
    * Immediate 3D objects are drawn on top(with depth from G-Buffer)
    * Immediate 2D objects are drawn on top

## Current requirements to build development source code

1) `vcpkg` (`@response_file.txt` installed for both `x64-windows-static` and `x86-windows-static`)
2) CMake 3.15+ (plus Ninja for single-config CTest builds)
3) LunarG Vulkan SDK (`VULKAN_SDK` set, providing `Bin/glslangValidator.exe`)
4) Visual Studio C++ toolchain with C++20 support
5) GTA San Andreas `v1.0 US` + [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (`winmm.dll`, Win32)

## Build, Test & Live Game Deployment Workflow

Because GTA San Andreas is a 32-bit (`x86`) executable while the Vulkan ray-tracing driver runs out-of-process as a 64-bit (`x64`) executable over shared memory (`RenderHookTaskQueue`), build the two CMake trees separately and deploy non-destructively to a GTA SA `v1.0 US` installation:

1. **Install `vcpkg` dependencies** (listed in `response_file.txt`) for both `x64-windows-static` and `x86-windows-static`.
2. **Build the 64-bit Vulkan render driver (`gta_sa_render_driver.exe`):**
   ```powershell
   cmake -S . -B ../build/vs_x64 -G "Visual Studio 18 2026" -A x64 `
     -DCMAKE_BUILD_TYPE=RelWithDebInfo `
     -DCMAKE_TOOLCHAIN_FILE="<vcpkg_root>/scripts/buildsystems/vcpkg.cmake" `
     -DVCPKG_TARGET_TRIPLET=x64-windows-static `
     -DVCPKG_APPLOCAL_DEPS=OFF `
     -DARCH_64BIT=ON -DBUILD_32BIT_LIBS=ON `
     -DGTA_SA_FOLDER="../build/staging_gtasa"
   cmake --build ../build/vs_x64 --config RelWithDebInfo --target gta_sa_render_driver
   ```
3. **Build the 32-bit GTA SA ASI hook (`gta_sa_render_hook.asi`):**
   ```powershell
   cmake -S . -B ../build/vs_x86 -G "Visual Studio 18 2026" -A Win32 `
     -DCMAKE_BUILD_TYPE=RelWithDebInfo `
     -DCMAKE_TOOLCHAIN_FILE="<vcpkg_root>/scripts/buildsystems/vcpkg.cmake" `
     -DVCPKG_TARGET_TRIPLET=x86-windows-static `
     -DVCPKG_APPLOCAL_DEPS=OFF `
     -DARCH_64BIT=OFF -DBUILD_32BIT_LIBS=ON `
     -DGTA_SA_FOLDER="../build/staging_gtasa"
   cmake --build ../build/vs_x86 --config RelWithDebInfo --target gta_sa_render_hook
   ```
   > Keep `-DVCPKG_APPLOCAL_DEPS=OFF` so a single-bitness `vulkan-1.dll` is never copied into the game folder shared by 32-bit `gta_sa.exe` (`SysWOW64\vulkan-1.dll`) and 64-bit `gta_sa_render_driver.exe` (`System32\vulkan-1.dll`).
4. **Build and run unit, shader, and GPU tests (`build_x64` with Ninja):**
   ```powershell
   # Inside an x64 MSVC developer environment (vcvars64.bat):
   cmake -S . -B ../build_x64 -G Ninja `
     -DCMAKE_BUILD_TYPE=RelWithDebInfo `
     -DCMAKE_TOOLCHAIN_FILE="<vcpkg_root>/scripts/buildsystems/vcpkg.cmake" `
     -DVCPKG_TARGET_TRIPLET=x64-windows-static `
     -DARCH_64BIT=ON -DBUILD_TESTS=ON
   pwsh ./tools/test.ps1 -All
   ```
5. **Redeploy to the live GTA SA `v1.0 US` folder for continuous testing after your code changes:**
   ```powershell
   # Initial deploy (Ultimate ASI Loader winmm.dll + RenderHook runtime package):
   pwsh ./tools/deploy_to_game.ps1 -GameDir "<path_to_gta_sa>" -InstallAsiLoader

   # Subsequent code/shader iterations (updates only RenderHook binaries, shaders, and resources):
   pwsh ./tools/deploy_to_game.ps1 -GameDir "<path_to_gta_sa>"
   ```
   `tools/deploy_to_game.ps1` verifies PE architectures (`0x014C` x86 ASI + `0x8664` x64 driver), preserves all original GTA SA files (`gta_sa.exe`, `vorbisFile.dll`, `vorbis.dll`, `ogg.dll`, `eax.dll`, `stream.ini`) untouched, and blocks conflicting deployment alongside the legacy DirectX 11 `gtaRenderHook.asi`.

## Tips

1) If you encounter bug please report it, and attach .log files and crash dumps and build/game version if possible to
   bugreport
2) It's buggy and code is bad at the moment, sorry in advance :)
