//
// Created by peter on 19.01.2021.
//
#pragma once
#include "common_headers.h"
#include <ipc/MemoryReader.h>
#include <ipc/MemoryWriter.h>

#include <cstdint>

namespace rh::rw::engine
{
class SharedMemoryTaskQueue;
class StartSystemCmdImpl
{
  public:
    StartSystemCmdImpl( SharedMemoryTaskQueue &task_queue );
    bool        Invoke( HWND window );
    static void RegisterCallHandler( SharedMemoryTaskQueue &task_queue );

    // WOW64 window handles carry 32 significant low bits zero-extended to 64
    // bits across processes; write and read a fixed 8-byte slot in shared
    // memory so the 64-bit driver never reads leftover upper bytes from other
    // tasks.
    static void SerializeWindowHandle( MemoryWriter &memory_writer,
                                       HWND          window )
    {
        uint64_t raw_handle =
            static_cast<uint64_t>( reinterpret_cast<uintptr_t>( window ) );
        memory_writer.Write( &raw_handle );
    }

    static HWND DeserializeWindowHandle( MemoryReader &memory_reader )
    {
        const uint64_t raw_handle = *memory_reader.Read<uint64_t>();
        return reinterpret_cast<HWND>( static_cast<uintptr_t>( raw_handle ) );
    }

  private:
    SharedMemoryTaskQueue &TaskQueue;
};
} // namespace rh::rw::engine