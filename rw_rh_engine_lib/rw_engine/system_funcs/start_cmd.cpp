//
// Created by peter on 19.01.2021.
//
#include "start_cmd.h"
#include <DebugUtils/DebugLogger.h>
#include <Engine/Common/IDeviceState.h>
#include <ipc/shared_memory_queue_client.h>
#include <render_client/imgui_state_recorder.h>
#include <render_driver/render_driver.h>

namespace rh::rw::engine
{

StartSystemCmdImpl::StartSystemCmdImpl( SharedMemoryTaskQueue &task_queue )
    : TaskQueue( task_queue )
{
}

bool StartSystemCmdImpl::Invoke( HWND window )
{
    TaskQueue.ExecuteTask( SharedMemoryTaskType::CREATE_WINDOW,
                           [window]( MemoryWriter &&memory_writer ) {
                               // serialize
                               SerializeWindowHandle( memory_writer, window );
                           } );
    InstallWinProcHook();
    return true;
}

void CreateWindowTaskImpl( void *memory )
{
    MemoryReader reader( memory );
    MemoryWriter writer( memory );

    // execute
    HWND wnd = StartSystemCmdImpl::DeserializeWindowHandle( reader );
    if ( !gRenderDriver->OpenMainWindow( wnd ) )
    {
        debug::DebugLogger::ErrorFmt(
            "Failed to open window:%llX",
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>( wnd ) ) );
        return;
    }
}

void StartSystemCmdImpl::RegisterCallHandler(
    SharedMemoryTaskQueue &task_queue )
{
    task_queue.RegisterTask(
        SharedMemoryTaskType::CREATE_WINDOW,
        std::make_unique<SharedMemoryTask>( CreateWindowTaskImpl ) );
}

} // namespace rh::rw::engine