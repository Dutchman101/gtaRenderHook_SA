//
// Created by peter on 20.01.2021.
//

#include "render_client.h"
#include <DebugUtils/DebugLogger.h>
#include <Engine/EngineConfigBlock.h>
#include <filesystem>
#include <rw_engine/rh_backend/material_backend.h>
#include <rw_engine/rh_backend/raster_backend.h>

namespace rh::rw::engine
{
std::unique_ptr<RenderClient> gRenderClient = nullptr;

/**
 * RenderClient RenderWare plugins storage
 */
class ClientPlugins
{
  public:
    explicit ClientPlugins( const PluginPtrTable &plugin_cb )
        : Raster( plugin_cb ), Material( plugin_cb )
    {
    }

  private:
    BackendRasterPlugin   Raster;
    BackendMaterialPlugin Material;
};

RenderClient::RenderClient()
{
    /// initialize SM task queue
    TaskQueue =
        std::make_unique<SharedMemoryTaskQueue>( SharedMemoryTaskQueueInfo{
            .mName = "RenderHookTaskQueue",
            .mSize = 1024 * 1024 *
                     rh::engine::EngineConfigBlock::It.SharedMemorySizeMB,
            .mOwner = true } );

    if ( IPCSettings::mMode != IPCRenderMode::CrossProcessClient )
        return;

    /// Create render driver sub-process
    // Resolve relative driver names to the host executable folder and run the
    // driver in its own directory so launching gta_sa.exe from another working
    // directory still finds sibling shaders and resources/blue_noise.bmp.
    std::string module_path( 256, '\0' );
    DWORD       module_path_len = 0;
    while ( module_path.size() <= 32768 )
    {
        module_path_len = GetModuleFileNameA(
            nullptr, module_path.data(),
            static_cast<DWORD>( module_path.size() ) );
        if ( module_path_len == 0 || module_path_len < module_path.size() )
            break;
        module_path.resize( module_path.size() * 2, '\0' );
    }

    std::filesystem::path exe_dir{};
    if ( module_path_len > 0 && module_path_len < module_path.size() )
    {
        module_path.resize( module_path_len );
        exe_dir = std::filesystem::path( module_path ).parent_path();
    }

    std::filesystem::path driver_path( IPCSettings::mProcessName );
    if ( !driver_path.empty() && driver_path.is_relative() && !exe_dir.empty() )
        driver_path = exe_dir / driver_path;

    const std::filesystem::path working_dir_path =
        driver_path.has_parent_path() ? driver_path.parent_path() : exe_dir;
    const std::string driver_path_str = driver_path.string();
    const std::string working_dir_str = working_dir_path.string();
    const char       *working_dir =
        working_dir_str.empty() ? nullptr : working_dir_str.c_str();

    STARTUPINFOA start_info{ .cb = sizeof( start_info ) };
    if ( driver_path_str.empty() ||
         !CreateProcessA( driver_path_str.c_str(), nullptr, nullptr, nullptr,
                          FALSE, 0, nullptr, working_dir, &start_info,
                          &RenderDriverProcess ) )
    {
        const DWORD create_err =
            driver_path_str.empty() ? ERROR_INVALID_PARAMETER : GetLastError();
        debug::DebugLogger::ErrorFmt(
            "Failed to start render driver process (%s), error code: %lu",
            driver_path_str.c_str(), static_cast<unsigned long>( create_err ) );
    }
    else
    {
        TaskQueue->SetPeerProcess( RenderDriverProcess.hProcess );
    }
}

RenderClient::~RenderClient()
{
    if ( TaskQueue )
        TaskQueue->SendExitEvent();

    // Keep TaskQueue mapped until the driver exits so named IPC handles remain
    // valid while its 1000 ms task loop drains and shuts down Vulkan cleanly.
    if ( RenderDriverProcess.hProcess )
    {
        if ( WaitForSingleObject( RenderDriverProcess.hProcess, 2000 ) !=
             WAIT_OBJECT_0 )
        {
            TerminateProcess( RenderDriverProcess.hProcess, 0 );
        }
        if ( TaskQueue )
            TaskQueue->SetPeerProcess( nullptr );
        CloseHandle( RenderDriverProcess.hProcess );
        RenderDriverProcess.hProcess = nullptr;
    }
    if ( RenderDriverProcess.hThread )
    {
        CloseHandle( RenderDriverProcess.hThread );
        RenderDriverProcess.hThread = nullptr;
    }

    TaskQueue.reset();
}

bool RenderClient::RegisterPlugins( const PluginPtrTable &plugin_cb )
{
    Plugins = std::make_unique<ClientPlugins>( plugin_cb );
    return true;
}

} // namespace rh::rw::engine