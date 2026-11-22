#pragma once

#include <atomic>
#include <cstdint>

// stdafx.h includes this before the logger exists, so writers report through a sink the host installs.
using TPatchFailureSink = void ( * )( const char *operation, void *address, const char *reason );
inline std::atomic<TPatchFailureSink> g_pPatchFailureSink{ nullptr };
inline void SetPatchFailureSink( TPatchFailureSink sink )
{
    g_pPatchFailureSink.store( sink, std::memory_order_release );
}

// Every refusal passes here, so a host can report once that a batch did not finish. Reporting only, nothing reads it.
inline std::atomic<unsigned long> g_ulPatchRefusals{ 0 };
inline unsigned long PatchRefusalCount()
{
    return g_ulPatchRefusals.load( std::memory_order_acquire );
}
inline void DeliverPatchFailureLine( const char *operation, void *address, const char *reason )
{
    const TPatchFailureSink pfnSink = g_pPatchFailureSink.load( std::memory_order_acquire );
    if ( pfnSink != nullptr )
        pfnSink( operation, address, reason );
}
inline void ReportPatchFailure( const char *operation, void *address, const char *reason )
{
    g_ulPatchRefusals.fetch_add( 1, std::memory_order_acq_rel );
    DeliverPatchFailureLine( operation, address, reason );
}

// Bytes are in but the flush or restore failed: reported but not counted.
inline void ReportPatchWrittenNotClean( const char *operation, void *address, const char *reason )
{
    DeliverPatchFailureLine( operation, address, reason );
}

// A finished write has its own sink, so a live line never reads as a refusal.
using TPatchSuccessSink = void ( * )( const char *operation, void *address );
inline std::atomic<TPatchSuccessSink> g_pPatchSuccessSink{ nullptr };
inline void SetPatchSuccessSink( TPatchSuccessSink sink )
{
    g_pPatchSuccessSink.store( sink, std::memory_order_release );
}
inline void ReportPatchSuccess( const char *operation, void *address )
{
    const TPatchSuccessSink pfnSink = g_pPatchSuccessSink.load( std::memory_order_acquire );
    if ( pfnSink != nullptr )
        pfnSink( operation, address );
}

enum class EMemoryAccess
{
    Readable,
    Writable,
    Executable
};

// True when the whole range sits in one committed region.
inline bool IsSingleCommittedRegion( const void *address, SIZE_T size, MEMORY_BASIC_INFORMATION &region )
{
    MEMORY_BASIC_INFORMATION lastRegion{};
    region = MEMORY_BASIC_INFORMATION{};
    if ( size == 0 )
        return false;
    const UINT_PTR start = reinterpret_cast<UINT_PTR>( address );
    const UINT_PTR lastByte = start + ( size - 1 );
    if ( lastByte < start || VirtualQuery( address, &region, sizeof( region ) ) == 0 ||
         VirtualQuery( reinterpret_cast<const void *>( lastByte ), &lastRegion, sizeof( lastRegion ) ) == 0 )
        return false;
    return region.BaseAddress == lastRegion.BaseAddress && region.State == MEM_COMMIT;
}

// The access type is the low byte; guard, no cache and write combine all sit above this mask.
constexpr DWORD PAGE_ACCESS_TYPE_MASK = 0xFF;

inline bool IsExecutableProtection( DWORD protection )
{
    switch ( protection & PAGE_ACCESS_TYPE_MASK )
    {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

// Read only answers a load and faults on a store, so a page that passes this can still fail a write.
inline bool IsWritableProtection( DWORD protection )
{
    switch ( protection & PAGE_ACCESS_TYPE_MASK )
    {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

inline bool IsReadableProtection( DWORD protection )
{
    switch ( protection & PAGE_ACCESS_TYPE_MASK )
    {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

inline bool IsCommittedRange( const void *address, SIZE_T size, EMemoryAccess access )
{
    MEMORY_BASIC_INFORMATION info{};
    if ( !IsSingleCommittedRegion( address, size, info ) )
        return false;
    if ( ( info.Protect & PAGE_GUARD ) != 0 )
        return false;
    switch ( access )
    {
    case EMemoryAccess::Executable:
        return IsExecutableProtection( info.Protect );
    case EMemoryAccess::Writable:
        return IsWritableProtection( info.Protect );
    case EMemoryAccess::Readable:
        return IsReadableProtection( info.Protect );
    }
    return false;
}

inline bool IsReadableRange( const void *address, SIZE_T size )
{
    return IsCommittedRange( address, size, EMemoryAccess::Readable );
}

inline bool IsExecutableRange( const void *address, SIZE_T size )
{
    return IsCommittedRange( address, size, EMemoryAccess::Executable );
}

inline bool IsWritableRange( const void *address, SIZE_T size )
{
    return IsCommittedRange( address, size, EMemoryAccess::Writable );
}

// Where the executable is linked. Every fixed site here is a link time address.
constexpr UINT_PTR HOST_IMAGE_LOAD_ADDRESS = 0x00400000;

inline bool GetHostImageRange( UINT_PTR &imageBase, UINT_PTR &imageSize )
{
    imageBase = 0;
    imageSize = 0;
    const unsigned char *pImage = reinterpret_cast<const unsigned char *>( GetModuleHandleA( nullptr ) );
    if ( pImage == nullptr || !IsReadableRange( pImage, sizeof( IMAGE_DOS_HEADER ) ) )
        return false;
    const auto *pDosHeader = reinterpret_cast<const IMAGE_DOS_HEADER *>( pImage );
    if ( pDosHeader->e_magic != IMAGE_DOS_SIGNATURE || pDosHeader->e_lfanew <= 0 )
        return false;
    // Integer sum with a wrap refusal, because e_lfanew comes out of a header.
    const UINT_PTR imageAddress = reinterpret_cast<UINT_PTR>( pImage );
    const UINT_PTR ntAddress = imageAddress + static_cast<UINT_PTR>( pDosHeader->e_lfanew );
    if ( ntAddress < imageAddress )
        return false;
    const auto *pNtHeaders = reinterpret_cast<const IMAGE_NT_HEADERS *>( ntAddress );
    if ( !IsReadableRange( pNtHeaders, sizeof( IMAGE_NT_HEADERS ) ) ||
         pNtHeaders->Signature != IMAGE_NT_SIGNATURE )
        return false;
    imageBase = imageAddress;
    imageSize = pNtHeaders->OptionalHeader.SizeOfImage;
    return imageSize != 0;
}

// Four outcomes: a relocated image isnt a site that left the image, and one bool would report it as one.
enum class EHostImage
{
    Unknown,
    Relocated,
    Contained,
    Outside
};

inline EHostImage ClassifyHostImage( const void *address, SIZE_T size )
{
    UINT_PTR imageBase = 0;
    UINT_PTR imageSize = 0;
    if ( !GetHostImageRange( imageBase, imageSize ) )
        return EHostImage::Unknown;
    if ( imageBase != HOST_IMAGE_LOAD_ADDRESS )
        return EHostImage::Relocated;
    if ( size == 0 )
        return EHostImage::Outside;
    const UINT_PTR start = reinterpret_cast<UINT_PTR>( address );
    const bool bContained =
        start >= imageBase && start - imageBase <= imageSize && size <= imageSize - ( start - imageBase );
    return bContained ? EHostImage::Contained : EHostImage::Outside;
}

// Code elsewhere in the process passes every protection test, so this is the only test that binds a site to the
// image.
inline bool IsInHostImage( const void *address, SIZE_T size )
{
    return ClassifyHostImage( address, size ) == EHostImage::Contained;
}

enum class EWriteResult
{
    Failed,
    WrittenNotClean,
    Clean
};

// A thread id is reused once a thread exits, so a token that never repeats is combined with it.
inline std::atomic<unsigned long> g_ulNextPatchWriterToken{ 1 };

inline unsigned long PatchWriterToken()
{
    static thread_local const unsigned long s_ulToken = g_ulNextPatchWriterToken.fetch_add( 1 );
    return s_ulToken;
}

// Released after each write, so one contended write doesnt lock out every later writer.
inline std::atomic<bool> g_bPatchWriterBusy{ false };
inline std::atomic<unsigned long long> g_ullPatchWriter{ 0 };
inline unsigned long long PatchWriterIdentity()
{
    return ( static_cast<unsigned long long>( PatchWriterToken() ) << 32 ) |
           static_cast<unsigned long long>( GetCurrentThreadId() );
}
inline unsigned long PatchWriterIdFromIdentity( unsigned long long identity )
{
    return static_cast<unsigned long>( identity & 0xFFFFFFFFULL );
}

struct SPatchWriterSlot
{
    bool m_bHeld = false;

    SPatchWriterSlot() = default;
    SPatchWriterSlot( const SPatchWriterSlot & ) = delete;
    SPatchWriterSlot & operator=( const SPatchWriterSlot & ) = delete;
    ~SPatchWriterSlot()
    {
        Release();
    }

    bool Acquire( unsigned long long identity )
    {
        bool bExpected = false;
        if ( !g_bPatchWriterBusy.compare_exchange_strong( bExpected, true, std::memory_order_acq_rel ) )
            return false;
        g_ullPatchWriter.store( identity, std::memory_order_relaxed );
        m_bHeld = true;
        return true;
    }

    // Identity cleared before busy, so the next taker cant have it wiped by this release.
    void Release()
    {
        if ( !m_bHeld )
            return;
        m_bHeld = false;
        g_ullPatchWriter.store( 0, std::memory_order_relaxed );
        g_bPatchWriterBusy.store( false, std::memory_order_release );
    }
};

template <typename TWriter>
inline EWriteResult WriteProtected( const char *operation, void *address, SIZE_T size, TWriter write )
{
    unsigned long protect[2]{};
    MEMORY_BASIC_INFORMATION region{};
    if ( !IsSingleCommittedRegion( address, size, region ) )
    {
        ReportPatchFailure( operation, address, "does not sit inside one committed region, so the protection it "
                                                 "would be given back after the write is not known" );
        return EWriteResult::Failed;
    }
    // Held across the copy: two threads rewriting one site leave a torn instruction.
    const unsigned long long ullWriter = PatchWriterIdentity();
    SPatchWriterSlot writerSlot;
    if ( !writerSlot.Acquire( ullWriter ) )
    {
        const unsigned long long ullHolder = g_ullPatchWriter.load( std::memory_order_relaxed );
        char refusal[256];
        if ( ullHolder != 0 )
            _snprintf_s( refusal, sizeof( refusal ), _TRUNCATE,
                         "was not written because thread %lu holds this process's single write slot, so a write from "
                         "thread %lu was refused rather than risk a torn instruction",
                         PatchWriterIdFromIdentity( ullHolder ), PatchWriterIdFromIdentity( ullWriter ) );
        else
            _snprintf_s( refusal, sizeof( refusal ), _TRUNCATE,
                         "was not written because another thread had just taken this process's single write slot, so a "
                         "write from thread %lu was refused rather than risk a torn instruction",
                         PatchWriterIdFromIdentity( ullWriter ) );
        ReportPatchFailure( operation, address, refusal );
        return EWriteResult::Failed;
    }
    // Write access only, so a data page isnt made executable; a copy on write page isnt already writable.
    const bool bGuarded = ( region.Protect & PAGE_GUARD ) != 0;
    const DWORD dwProtection = region.Protect & PAGE_ACCESS_TYPE_MASK;
    const bool bAlreadyWritable =
        !bGuarded && ( dwProtection == PAGE_READWRITE || dwProtection == PAGE_EXECUTE_READWRITE );
    const DWORD dwWriteProtect =
        ( !bGuarded && IsExecutableProtection( dwProtection ) ) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if ( !bAlreadyWritable && !VirtualProtect( address, size, dwWriteProtect, &protect[0] ) )
    {
        // Released first, so this report runs with the slot free and a sink that patched is not refused it here.
        writerSlot.Release();
        ReportPatchFailure( operation, address, "could not be made writable, so it was left as it was" );
        return EWriteResult::Failed;
    }
    write( address );
    // The CPU caches code bytes, so it has to be told, and the page has to go back to the protection it had.
    const bool bFlushed = FlushInstructionCache( GetCurrentProcess(), address, size ) != FALSE;
    // protect[0] still carries PAGE_GUARD, so the guard comes back with it.
    const bool bRestored = bAlreadyWritable || VirtualProtect( address, size, protect[0], &protect[1] ) != FALSE;
    // Released before reporting, so a sink that patched could take the slot.
    writerSlot.Release();
    if ( !bFlushed )
        ReportPatchWrittenNotClean( operation, address, "was written but the processor was not told it changed, so it "
                                                       "may keep using the copy it already had" );
    if ( !bRestored )
        ReportPatchWrittenNotClean( operation, address, "was written but could not be given back its own protection, "
                                                       "so the page keeps the write access this call added" );
    if ( !bFlushed || !bRestored )
        return EWriteResult::WrittenNotClean;
    ReportPatchSuccess( operation, address );
    return EWriteResult::Clean;
}

// The write's own result, not a bool: a bool cant tell a refusal from an unreported write.
inline EWriteResult Patch( const char *operation, void *address, const void *data, SIZE_T size )
{
    return WriteProtected( operation, address, size,
                           [data, size] ( void *destination ) { memcpy( destination, data, size ); } );
}

// Unsigned: a signed value cant hold an address at or above 0x80000000, and the subtraction is defined wrap.
inline EWriteResult SetPointer( UINT_PTR address, void *value )
{
    return Patch( "SetPointer", reinterpret_cast<void *>( address ), &value, sizeof( void * ) );
}
inline EWriteResult SetInt( UINT_PTR address, int value )
{
    return Patch( "SetInt", reinterpret_cast<void *>( address ), &value, sizeof( int ) );
}

// A writer replaces opcode and displacement together, while a reader addresses the operand one byte past the opcode.
constexpr int REL32_OPCODE_LENGTH       = 1;
constexpr int REL32_DISPLACEMENT_LENGTH = static_cast<int>( sizeof( uint32_t ) );
constexpr int REL32_BRANCH_LENGTH       = REL32_OPCODE_LENGTH + REL32_DISPLACEMENT_LENGTH;

static_assert( REL32_OPCODE_LENGTH == 1,
               "E8 and E9 are one byte opcodes, so every operand addressed from one byte past them moves if this "
               "changes" );

constexpr unsigned char REL32_CALL_OPCODE = 0xE8;
constexpr unsigned char REL32_JUMP_OPCODE = 0xE9;

constexpr unsigned char PUSH_IMM32_OPCODE         = 0x68;
constexpr int PUSH_IMM32_OPCODE_LENGTH            = 1;
constexpr int PUSH_IMM32_IMMEDIATE_LENGTH         = static_cast<int>( sizeof( uint32_t ) );
constexpr int PUSH_IMM32_LENGTH                   = PUSH_IMM32_OPCODE_LENGTH + PUSH_IMM32_IMMEDIATE_LENGTH;

static_assert( PUSH_IMM32_OPCODE_LENGTH == 1,
               "68h is a one byte opcode, so a guard that covers this instruction, the operand read one byte past "
               "it and the operand write there all move if this changes" );

// In 32 bits a rel32 branch reaches every address the process can hold, so the wrapped difference names no wrong
// place.
static_assert( sizeof( UINT_PTR ) == sizeof( uint32_t ),
               "the wrapped displacement below reaches every target only while an address is the width of the operand, so "
               "a target wider than the operand has to test the difference before it is stored" );

inline uint32_t EncodeRel32( UINT_PTR address, void *func )
{
    const uint32_t next = static_cast<uint32_t>( address ) + REL32_BRANCH_LENGTH;
    return static_cast<uint32_t>( reinterpret_cast<UINT_PTR>( func ) ) - next;
}
inline uint32_t ReadRel32Displacement( UINT_PTR address )
{
    uint32_t displacement;
    static_assert( static_cast<int>( sizeof( displacement ) ) == REL32_DISPLACEMENT_LENGTH,
                   "every caller proves REL32_BRANCH_LENGTH readable before this reads, so the operand copied here "
                   "has to be exactly the length that guard covers past the opcode" );
    memcpy( &displacement, reinterpret_cast<const void *>( address + REL32_OPCODE_LENGTH ),
            sizeof( displacement ) );
    return displacement;
}
inline bool HoldsOwnBranch( UINT_PTR address, unsigned char opcode, void *func )
{
    if ( !IsReadableRange( reinterpret_cast<const void *>( address ), REL32_BRANCH_LENGTH ) )
        return false;
    return *reinterpret_cast<const unsigned char *>( address ) == opcode &&
           ReadRel32Displacement( address ) == EncodeRel32( address, func );
}
// Unreadable and different report apart: the first is a site that moved, the second is code the caller wasnt built
// against.
enum class EBytesMatch
{
    Unreadable,
    Mismatch,
    Match
};

inline EBytesMatch CompareExpectedBytes( UINT_PTR address, const void *expected, SIZE_T length )
{
    if ( !IsReadableRange( reinterpret_cast<const void *>( address ), length ) )
        return EBytesMatch::Unreadable;
    return memcmp( reinterpret_cast<const void *>( address ), expected, length ) == 0
               ? EBytesMatch::Match
               : EBytesMatch::Mismatch;
}

inline bool HoldsExpectedBytes( UINT_PTR address, const void *expected, SIZE_T length )
{
    return CompareExpectedBytes( address, expected, length ) == EBytesMatch::Match;
}
inline EWriteResult InstallCall( UINT_PTR address, void *func )
{
    if ( !IsReadableRange( reinterpret_cast<const void *>( address ), REL32_BRANCH_LENGTH ) )
    {
        ReportPatchFailure( "InstallCall", reinterpret_cast<void *>( address ),
                            "is not five readable bytes inside one committed region, so no call was written "
                            "there" );
        return EWriteResult::Failed;
    }
    if ( *reinterpret_cast<const unsigned char *>( address ) != REL32_CALL_OPCODE )
    {
        ReportPatchFailure( "InstallCall", reinterpret_cast<void *>( address ),
                            "does not hold the call this hook replaces, so nothing was written there" );
        return EWriteResult::Failed;
    }
    // Readable is not executable, and only this test covers the bytes being code.
    if ( !IsExecutableRange( reinterpret_cast<const void *>( address ), REL32_BRANCH_LENGTH ) )
    {
        ReportPatchFailure( "InstallCall", reinterpret_cast<void *>( address ),
                            "is not memory this process can execute, so no call was written there" );
        return EWriteResult::Failed;
    }
    // One write for all five bytes; split, the site would hold a new opcode beside the old operand.
    const uint32_t rel = EncodeRel32( address, func );
    unsigned char call[REL32_BRANCH_LENGTH];
    static_assert( REL32_OPCODE_LENGTH + static_cast<int>( sizeof( rel ) ) == REL32_BRANCH_LENGTH,
                   "the operand copy starts after the opcode and has to end exactly at the end of this buffer, or it "
                   "writes past it or leaves a byte of the site holding the old operand" );
    call[0] = REL32_CALL_OPCODE;
    memcpy( call + REL32_OPCODE_LENGTH, &rel, sizeof( rel ) );
    // WriteProtected directly, so an unseen write isnt flattened to a success.
    return WriteProtected( "InstallCall", reinterpret_cast<void *>( address ), sizeof( call ),
                           [&call] ( void *destination ) { memcpy( destination, call, sizeof( call ) ); } );
}

inline EWriteResult InstallJump( UINT_PTR address, void *func, UINT_PTR *pChainTarget = nullptr )
{
    // Tested before the site is read for a jump, so one site reaches one verdict.
    if ( !IsExecutableRange( reinterpret_cast<const void *>( address ), REL32_BRANCH_LENGTH ) )
    {
        ReportPatchFailure( "RedirectJump", reinterpret_cast<void *>( address ),
                            "is not memory this process can execute, so no jump was written there" );
        return EWriteResult::Failed;
    }
    // Kept until the write lands, so a failed write never leaves the target naming a hook we didnt displace.
    UINT_PTR ullChainTarget = 0;
    // Written as they are, without decoding what they replace. Anything resuming at address + 5 has to decode first.
    if ( IsReadableRange( reinterpret_cast<const void *>( address ), REL32_BRANCH_LENGTH ) &&
         *reinterpret_cast<const unsigned char *>( address ) == REL32_JUMP_OPCODE )
    {
        const UINT_PTR existingTarget = address + REL32_BRANCH_LENGTH + ReadRel32Displacement( address );
        // This hooks own branch, so a second pass over an already patched image is a no-op rather than a rewrite.
        if ( existingTarget == reinterpret_cast<UINT_PTR>( func ) )
        {
            // Nothing displaced here, so the caller is told zero.
            if ( pChainTarget != nullptr )
                *pChainTarget = 0;
            return EWriteResult::Clean;
        }
        // A jump leaving the host image isnt one of the executable's own thunks, so another module put it there.
        switch ( ClassifyHostImage( reinterpret_cast<const void *>( existingTarget ), REL32_BRANCH_LENGTH ) )
        {
        case EHostImage::Contained:
            // In-image targets cannot be chained: calling one runs that code over the running frame.
            if ( pChainTarget != nullptr )
            {
                ReportPatchFailure( "RedirectJump", reinterpret_cast<void *>( address ),
                                    "already holds a jump to a target inside this executable's own image, which cannot "
                                    "be chained, so that site was left in place rather than taken over without the "
                                    "hook it holds" );
                return EWriteResult::Failed;
            }
            break;
        case EHostImage::Outside:
            // Only a caller that can run the displaced hook asks to chain, so the others keep the refusal.
            if ( pChainTarget == nullptr )
            {
                ReportPatchFailure( "RedirectJump", reinterpret_cast<void *>( address ),
                                    "already holds a jump that leaves this executable's image, so another module owns "
                                    "that site and its hook was left in place" );
                return EWriteResult::Failed;
            }
            // Decoded from the site, so it names code only if that page is executable.
            if ( !IsExecutableRange( reinterpret_cast<const void *>( existingTarget ), REL32_OPCODE_LENGTH ) )
            {
                ReportPatchFailure( "RedirectJump", reinterpret_cast<void *>( address ),
                                    "already holds a jump that leaves this executable's image, but the place it jumps "
                                    "to is not memory this process can execute, so that site was left in place rather "
                                    "than taken over and then called" );
                return EWriteResult::Failed;
            }
            ullChainTarget = existingTarget;
            break;
        case EHostImage::Relocated:
            ReportPatchFailure( "RedirectJump", reinterpret_cast<void *>( address ),
                                "already holds a jump whose target cannot be checked against this executable, because "
                                "the executable is not loaded at the address it was linked at, so its hook was left in "
                                "place rather than overwritten on a guess" );
            return EWriteResult::Failed;
        case EHostImage::Unknown:
            ReportPatchFailure( "RedirectJump", reinterpret_cast<void *>( address ),
                                "already holds a jump whose target could not be read back as part of this "
                                "executable's image, so its hook was left in place rather than overwritten on a guess" );
            return EWriteResult::Failed;
        }
    }
    const uint32_t rel = EncodeRel32( address, func );
    unsigned char jump[REL32_BRANCH_LENGTH];
    static_assert( REL32_OPCODE_LENGTH + static_cast<int>( sizeof( rel ) ) == REL32_BRANCH_LENGTH,
                   "the operand copy starts after the opcode and has to end exactly at the end of this buffer, or it "
                   "writes past it or leaves a byte of the site holding the old operand" );
    jump[0] = REL32_JUMP_OPCODE;
    memcpy( jump + REL32_OPCODE_LENGTH, &rel, sizeof( rel ) );
    const EWriteResult result = WriteProtected( "RedirectJump", reinterpret_cast<void *>( address ), sizeof( jump ),
                                                [&jump] ( void *destination ) { memcpy( destination, jump, sizeof( jump ) ); } );
    // Published only for a write that landed, and only to a caller that asked.
    if ( result != EWriteResult::Failed && pChainTarget != nullptr )
        *pChainTarget = ullChainTarget;
    return result;
}

inline EWriteResult RedirectJump( UINT_PTR address, void *func, UINT_PTR *pChainTarget = nullptr )
{
    return InstallJump( address, func, pChainTarget );
}
