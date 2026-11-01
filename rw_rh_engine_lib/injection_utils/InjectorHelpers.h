#pragma once
#include <Windows.h>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <type_traits>

static void Patch( INT_PTR address, void *data, SIZE_T size )
{
    auto adr_ptr = reinterpret_cast<void *>( address );

    unsigned long protect[2];
    VirtualProtect( adr_ptr, size, PAGE_EXECUTE_READWRITE, &protect[0] );
    memcpy( adr_ptr, data, size );
    FlushInstructionCache( GetCurrentProcess(), adr_ptr, size );
    VirtualProtect( adr_ptr, size, protect[0], &protect[1] );
}

/**
 * \brief
 * \param address
 * \param value
 */
static void SetPointer( INT_PTR address, void *value )
{
    Patch( address, &value, sizeof( void * ) );
}

inline static void SetInt( INT_PTR address, int value )
{
    Patch( address, &value, sizeof( int ) );
}

#if defined( _WIN64 )
inline static void *AllocateNearbyRelay( INT_PTR origin, void *target )
{
    SYSTEM_INFO sys_info{};
    GetSystemInfo( &sys_info );
    const uintptr_t gran = sys_info.dwAllocationGranularity
                               ? sys_info.dwAllocationGranularity
                               : 0x10000u;
    const uintptr_t base =
        ( static_cast<uintptr_t>( origin ) & ~( gran - 1u ) );
    constexpr uintptr_t max_range = 0x7FFF0000ULL;

    for ( uintptr_t offset = gran; offset < max_range; offset += gran )
    {
        const uintptr_t candidates[2] = {
            base + offset, ( base > offset ) ? ( base - offset ) : 0u };
        for ( uintptr_t addr : candidates )
        {
            if ( addr == 0u )
                continue;
            const intptr_t diff =
                static_cast<intptr_t>( addr ) - static_cast<intptr_t>( origin );
            if ( diff < INT32_MIN || diff > INT32_MAX )
                continue;
            auto *mem = static_cast<uint8_t *>(
                VirtualAlloc( reinterpret_cast<void *>( addr ), 16,
                              MEM_COMMIT | MEM_RESERVE,
                              PAGE_EXECUTE_READWRITE ) );
            if ( mem != nullptr )
            {
                const intptr_t actual_diff =
                    reinterpret_cast<intptr_t>( mem ) -
                    static_cast<intptr_t>( origin );
                if ( actual_diff < INT32_MIN || actual_diff > INT32_MAX )
                {
                    VirtualFree( mem, 0, MEM_RELEASE );
                    continue;
                }
                mem[0]         = 0xFF;
                mem[1]         = 0x25;
                mem[2]         = 0x00;
                mem[3]         = 0x00;
                mem[4]         = 0x00;
                mem[5]         = 0x00;
                const auto dst = reinterpret_cast<uint64_t>( target );
                memcpy( mem + 6, &dst, sizeof( dst ) );
                FlushInstructionCache( GetCurrentProcess(), mem, 14 );
                return mem;
            }
        }
    }
    return nullptr;
}
#endif

inline static void RedirectCall( INT_PTR address, void *func )
{
    void *target = func;
#if defined( _WIN64 )
    const intptr_t delta = reinterpret_cast<intptr_t>( target ) -
                           static_cast<intptr_t>( address + 5 );
    if ( delta < INT32_MIN || delta > INT32_MAX )
    {
        if ( void *relay = AllocateNearbyRelay( address + 5, target ) )
            target = relay;
    }
#endif
    unsigned char opcode = 0xE8;
    Patch( address, &opcode, sizeof( opcode ) );
    auto rel = static_cast<int32_t>( reinterpret_cast<intptr_t>( target ) -
                                     static_cast<intptr_t>( address + 5 ) );
    Patch( ( address + 1 ), &rel, sizeof( rel ) );
}

inline static void RedirectJump( INT_PTR address, void *func )
{
    void *target = func;
#if defined( _WIN64 )
    const intptr_t delta = reinterpret_cast<intptr_t>( target ) -
                           static_cast<intptr_t>( address + 5 );
    if ( delta < INT32_MIN || delta > INT32_MAX )
    {
        if ( void *relay = AllocateNearbyRelay( address + 5, target ) )
            target = relay;
    }
#endif
    unsigned char opcode = 0xE9;
    Patch( address, &opcode, sizeof( opcode ) );
    auto rel = static_cast<int32_t>( reinterpret_cast<intptr_t>( target ) -
                                     static_cast<intptr_t>( address + 5 ) );
    Patch( ( address + 1 ), &rel, sizeof( rel ) );
}

inline static void Nop( INT_PTR address, SIZE_T size )
{
    auto adr_ptr = reinterpret_cast<void *>( address );

    unsigned long protect[2];
    VirtualProtect( adr_ptr, size, PAGE_EXECUTE_READWRITE, &protect[0] );
    memset( adr_ptr, 0x90, size );
    VirtualProtect( adr_ptr, size, protect[0], &protect[1] );
}

template <typename RetType, uint32_t vtable_id, typename C, typename... Args>
auto InMemoryVirtualFunc( C _this, Args... args )
{
    using return_type = RetType( __thiscall * )( C, Args... );
    auto address      = ( *reinterpret_cast<void ***>( _this ) )[vtable_id];
    if constexpr ( std::is_void_v<RetType> )
        reinterpret_cast<return_type>( address )( _this, args... );
    else
        return reinterpret_cast<return_type>( address )( _this, args... );
}

template <typename RetType, typename... Args>
auto InMemoryFuncCall( uint32_t address, Args... args )
{
    using return_type = RetType( __cdecl * )( Args... );
    if constexpr ( std::is_void_v<RetType> )
        reinterpret_cast<return_type>( address )( args... );
    else
        return reinterpret_cast<return_type>( address )( args... );
}