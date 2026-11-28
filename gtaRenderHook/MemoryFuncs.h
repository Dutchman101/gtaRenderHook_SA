#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

// stdafx.h pulls this in before the logger exists, so writers report through a sink the host installs.
using TPatchFailureSink = void ( * )( const char *operation, void *address, const char *reason );
inline std::atomic<TPatchFailureSink> g_pPatchFailureSink{ nullptr };
inline void SetPatchFailureSink( TPatchFailureSink sink )
{
    g_pPatchFailureSink.store( sink, std::memory_order_release );
}

// Read once per batch, so a host can report that a load did not finish cleanly.
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

// Bytes landed but the flush or the protection restore failed: reported, not counted as a refusal.
inline void ReportPatchWrittenNotClean( const char *operation, void *address, const char *reason )
{
    DeliverPatchFailureLine( operation, address, reason );
}

// A finished write gets its own sink, so a healthy line never reads as a refusal.
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

// A third sink for a site handled correctly: a limit another tool already raised, or a site another module wrote before
// this plugin did. Counting either as a refusal would report a healthy load as a broken one.
using TPatchNoteSink = bool ( * )( const char *operation, const char *note );
inline std::atomic<TPatchNoteSink> g_pPatchNoteSink{ nullptr };

inline void SetPatchNoteSink( TPatchNoteSink sink )
{
    g_pPatchNoteSink.store( sink, std::memory_order_release );
}

// Whether every note handed to the host since BeginPatchNoteDeliveryCheck reached it. One global answer is sound because
// nothing else writes a note inside that window: a producer outside that sequence would describe someone else's note.
inline std::atomic<bool> g_bPatchNoteLanded{ true };
inline void BeginPatchNoteDeliveryCheck()
{
    g_bPatchNoteLanded.store( true, std::memory_order_release );
}
inline bool PatchNotesLanded()
{
    return g_bPatchNoteLanded.load( std::memory_order_acquire );
}

// Answers whether the note reached the host. No caller reads the answer: a failure here also clears the delivery marker
// above, and that marker is what the site audit reads. A host that installed no sink hasnt asked for this channel, so
// nothing is owed and nothing failed.
inline bool ReportPatchNote( const char *operation, const char *note )
{
    const TPatchNoteSink pfnSink = g_pPatchNoteSink.load( std::memory_order_acquire );
    if ( pfnSink == nullptr )
        return true;
    const bool bLanded = pfnSink( operation, note );
    if ( !bLanded )
        g_bPatchNoteLanded.store( false, std::memory_order_release );
    return bLanded;
}

// One slot for a note raised off the frame path: copied, since it is read after the producer is gone.
constexpr size_t LATCHED_NOTE_TEXT_LENGTH = 512;
constexpr size_t LATCHED_NOTE_OPERATION_LENGTH = 64;
inline std::atomic<bool> g_bLatchedNotePending{ false };

inline char *LatchedNoteOperation()
{
    static char s_szOperation[LATCHED_NOTE_OPERATION_LENGTH]{};
    return s_szOperation;
}

inline char *LatchedNoteText()
{
    static char s_szText[LATCHED_NOTE_TEXT_LENGTH]{};
    return s_szText;
}

// One writer and one reader on two threads. Publishing the pending flag orders the write before it but doesnt stop a
// second one while the frame path copies the first out, so the buffer itself needs this lock.
inline std::mutex &LatchedNoteMutex()
{
    static std::mutex s_mutex;
    return s_mutex;
}

inline void LatchPatchNote( const char *pOperation, const char *pNote )
{
    if ( pOperation == nullptr || pNote == nullptr )
        return;
    // Formatted under the slot mutex, so a second latch cant overwrite the buffer mid copy. The release store is what
    // makes the flag answer whether anything is waiting.
    {
        std::lock_guard<std::mutex> guard( LatchedNoteMutex() );
        _snprintf_s( LatchedNoteOperation(), LATCHED_NOTE_OPERATION_LENGTH, _TRUNCATE, "%s", pOperation );
        _snprintf_s( LatchedNoteText(), LATCHED_NOTE_TEXT_LENGTH, _TRUNCATE, "%s", pNote );
        g_bLatchedNotePending.store( true, std::memory_order_release );
    }
}

inline void DeliverLatchedPatchNote()
{
    // Unchecked first so a frame with nothing to deliver costs one load and no lock.
    if ( !g_bLatchedNotePending.load( std::memory_order_acquire ) )
        return;
    // Copied out under the producer's mutex, reported after it is released: the logger writes the file inside its own
    // lock, and a disk write is the last thing the streaming thread should wait on. Zeroed here so the buffer is a valid
    // empty string on every path out.
    char operation[LATCHED_NOTE_OPERATION_LENGTH]{};
    char note[LATCHED_NOTE_TEXT_LENGTH]{};
    {
        std::lock_guard<std::mutex> guard( LatchedNoteMutex() );
        if ( !g_bLatchedNotePending.load( std::memory_order_relaxed ) )
            return;
        _snprintf_s( operation, sizeof( operation ), _TRUNCATE, "%s", LatchedNoteOperation() );
        _snprintf_s( note, sizeof( note ), _TRUNCATE, "%s", LatchedNoteText() );
        // Cleared under the mutex: a latch after this point is delivered next frame, one before it is this copy.
        g_bLatchedNotePending.store( false, std::memory_order_relaxed );
    }
    ReportPatchNote( operation, note );
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

// Why a write was refused or left unclean. One per write, since the flags only ever get set.
struct SWriteRefusal
{
    bool m_bNotOneRegion = false;
    bool m_bWriteSlotContended = false;
    bool m_bNotWritable = false;
    bool m_bNotFlushed = false;
    bool m_bNotRestored = false;
};

// bReport is false for the writes the frame check repeats. Reporting those would reach the logger every restored frame
// for a write that already worked, and would read as a fresh install. A caller passing false owes its own line, and
// pRefusal is how it learns the reason.
template <typename TWriter>
inline EWriteResult WriteProtected( const char *operation, void *address, SIZE_T size, TWriter write,
                                    bool bReport = true, SWriteRefusal *pRefusal = nullptr )
{
    unsigned long protect[2]{};
    MEMORY_BASIC_INFORMATION region{};
    if ( !IsSingleCommittedRegion( address, size, region ) )
    {
        if ( pRefusal != nullptr )
            pRefusal->m_bNotOneRegion = true;
        if ( bReport )
            ReportPatchFailure( operation, address, "does not sit inside one committed region, so the protection it "
                                                     "would be given back after the write is not known" );
        return EWriteResult::Failed;
    }
    // Held across the copy: two threads rewriting one site leave a torn instruction.
    const unsigned long long ullWriter = PatchWriterIdentity();
    SPatchWriterSlot writerSlot;
    if ( !writerSlot.Acquire( ullWriter ) )
    {
        if ( pRefusal != nullptr )
            pRefusal->m_bWriteSlotContended = true;
        // Only built for a caller that reports it. The frame restores pass false, and wording a line nobody reads just
        // spends the frame loop on formatting.
        if ( bReport )
        {
            const unsigned long long ullHolder = g_ullPatchWriter.load( std::memory_order_relaxed );
            char refusal[256];
            if ( ullHolder != 0 )
                _snprintf_s( refusal, sizeof( refusal ), _TRUNCATE,
                             "was not written because thread %lu holds this process's single write slot, so a write "
                             "from thread %lu was refused rather than risk a torn instruction",
                             PatchWriterIdFromIdentity( ullHolder ), PatchWriterIdFromIdentity( ullWriter ) );
            else
                _snprintf_s( refusal, sizeof( refusal ), _TRUNCATE,
                             "was not written because another thread had just taken this process's single write slot, so "
                             "a write from thread %lu was refused rather than risk a torn instruction",
                             PatchWriterIdFromIdentity( ullWriter ) );
            ReportPatchFailure( operation, address, refusal );
        }
        return EWriteResult::Failed;
    }
    // Request write access only, so a data page isnt made executable. Both copy on write flags are excluded from the
    // already writable test even though IsWritableProtection counts them: the store is what creates this process's own
    // copy, the system leaves that copy writable, and restoring the recorded protection is what undoes it.
    const bool bGuarded = ( region.Protect & PAGE_GUARD ) != 0;
    const DWORD dwProtection = region.Protect & PAGE_ACCESS_TYPE_MASK;
    const bool bCopyOnWrite = dwProtection == PAGE_WRITECOPY || dwProtection == PAGE_EXECUTE_WRITECOPY;
    const bool bAlreadyWritable = !bGuarded && !bCopyOnWrite && IsWritableProtection( dwProtection );
    // Execute is kept on an executable page whether or not it is guarded: the guard is what gets lifted to write through
    // it, and dropping execute for that window faults any thread running elsewhere in the same region.
    const DWORD dwWriteProtect = IsExecutableProtection( dwProtection ) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    if ( !bAlreadyWritable && !VirtualProtect( address, size, dwWriteProtect, &protect[0] ) )
    {
        // Released first, so this report runs with the slot free and a sink that patched is not refused it here.
        writerSlot.Release();
        if ( pRefusal != nullptr )
            pRefusal->m_bNotWritable = true;
        if ( bReport )
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
    if ( !bFlushed && pRefusal != nullptr )
        pRefusal->m_bNotFlushed = true;
    if ( !bRestored && pRefusal != nullptr )
        pRefusal->m_bNotRestored = true;
    if ( !bFlushed && bReport )
        ReportPatchWrittenNotClean( operation, address, "was written but the processor was not told it changed, so it "
                                                       "may keep using the copy it already had" );
    if ( !bRestored && bReport )
        ReportPatchWrittenNotClean( operation, address, "was written but could not be given back its own protection, "
                                                       "so the page keeps the write access this call added" );
    if ( !bFlushed || !bRestored )
        return EWriteResult::WrittenNotClean;
    if ( bReport )
        ReportPatchSuccess( operation, address );
    return EWriteResult::Clean;
}

// The write's own result, not a bool: a bool cant tell a refusal from an unreported write.
inline EWriteResult Patch( const char *operation, void *address, const void *data, SIZE_T size,
                           bool bReport = true )
{
    return WriteProtected( operation, address, size,
                           [data, size] ( void *destination ) { memcpy( destination, data, size ); }, bReport );
}

// Unsigned, because a signed value cant hold an address at or above 0x80000000.
// The name defaults to the primitive; a caller that owns the site passes its own, so a refusal is reported under the
// site instead of under this helper.
inline EWriteResult SetPointer( UINT_PTR address, void *value, const char *pOperation = "SetPointer",
                                bool bReport = true )
{
    return Patch( pOperation, reinterpret_cast<void *>( address ), &value, sizeof( void * ), bReport );
}
inline EWriteResult SetInt( UINT_PTR address, int value, const char *pOperation = "SetInt",
                            bool bReport = true )
{
    return Patch( pOperation, reinterpret_cast<void *>( address ), &value, sizeof( int ), bReport );
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

// The other form the game hands an allocator its size in. Both streaming limit sites were read from the image: the byte
// before 0x5B8E55 is 68h and the one before 0x5B8EB0 is B9h, so a gate that accepted only the push would refuse the
// second limit.
constexpr unsigned char MOV_ECX_IMM32_OPCODE      = 0xB9;
constexpr int MOV_ECX_IMM32_OPCODE_LENGTH         = 1;
constexpr int MOV_ECX_IMM32_LENGTH                = MOV_ECX_IMM32_OPCODE_LENGTH + PUSH_IMM32_IMMEDIATE_LENGTH;

// C7 with a ModRM byte is how the game stores a pointer into an object. The ModRM and any displacement belong to the
// site, so only the opcode is named here.
constexpr unsigned char MOV_RM32_IMM32_OPCODE = 0xC7;

static_assert( MOV_ECX_IMM32_OPCODE_LENGTH == 1,
               "B9h is a one byte opcode, so the constant it carries starts one byte past it" );
static_assert( MOV_ECX_IMM32_LENGTH == PUSH_IMM32_LENGTH,
               "both forms carry one byte of opcode and the same four byte constant" );

static_assert( PUSH_IMM32_OPCODE_LENGTH == 1,
               "68h is a one byte opcode, so a guard covering this instruction, the operand read past it and the operand "
               "write there all move if this changes" );

// A four byte value inside an instruction may only be written again while that instruction is still the one this plugin
// was built against. These are the bytes read out of the game's own image at each site, running from the start of the
// instruction up to where the operand starts.
//
//   0x004C9AB5  68             push 0x004C9A80
//   0x005D67F4  68             push 0x005D6480
//   0x005D7B0B  68             push 0x005D77D0
//   0x005D9FE4  68             push 0x005D9900
//   0x005DAEC8  68             push 0x005DAC10   (the push is at 0x005DAEC7)
//                Nothing registers 0x005DAEC8, so this row records the bytes read there, not a site we write.
//   0x00815EDF  68             push 0x00815C20
//   0x00815F65  68             push 0x00815C20
//   0x00757899  C7 00          mov dword ptr [eax], 0x007578C0
//   0x007578AE  C7 40 0C       mov dword ptr [eax+0Ch], 0x00756DF0
//
// A value in the executable's data has no instruction around it, so it carries no prefix and is written back without a
// test. These are the prefixes recorded here, not every prefixed site in the tree.
struct SSiteInstructionPrefix
{
    const unsigned char *m_pBytes;
    unsigned char m_ucLength;
};

inline constexpr unsigned char SITE_PREFIX_PUSH_IMM32_BYTES[] = { PUSH_IMM32_OPCODE };
// C7h with ModRM 00h addresses eax with no displacement; 40h with 0Ch addresses the field 0Ch bytes in. Both are one
// site's own instruction, so the bytes are recorded here rather than named as the general encoding.
inline constexpr unsigned char SITE_PREFIX_MOV_EAX_IMM32_BYTES[] = { MOV_RM32_IMM32_OPCODE, 0x00 };
inline constexpr unsigned char SITE_PREFIX_MOV_EAX_DISP8_IMM32_BYTES[] = { MOV_RM32_IMM32_OPCODE, 0x40, 0x0C };
inline constexpr unsigned char SITE_PREFIX_MOV_ECX_IMM32_BYTES[] = { MOV_ECX_IMM32_OPCODE };

// Kept apart so neither answers the other: well formed means the bytes and the length agree about whether there is a
// prefix, present means there is one to test. A prefix that is present but not well formed can never be shown intact.
inline bool IsSitePrefixWellFormed( SSiteInstructionPrefix prefix )
{
    return ( prefix.m_pBytes == nullptr ) == ( prefix.m_ucLength == 0 );
}

inline bool HasSitePrefix( SSiteInstructionPrefix prefix )
{
    return prefix.m_pBytes != nullptr && prefix.m_ucLength != 0;
}

inline constexpr SSiteInstructionPrefix SITE_PREFIX_PUSH_IMM32{ SITE_PREFIX_PUSH_IMM32_BYTES,
                                                                sizeof( SITE_PREFIX_PUSH_IMM32_BYTES ) };
inline constexpr SSiteInstructionPrefix SITE_PREFIX_MOV_EAX_IMM32{ SITE_PREFIX_MOV_EAX_IMM32_BYTES,
                                                                   sizeof( SITE_PREFIX_MOV_EAX_IMM32_BYTES ) };
inline constexpr SSiteInstructionPrefix SITE_PREFIX_MOV_EAX_DISP8_IMM32{
    SITE_PREFIX_MOV_EAX_DISP8_IMM32_BYTES, sizeof( SITE_PREFIX_MOV_EAX_DISP8_IMM32_BYTES ) };
inline constexpr SSiteInstructionPrefix SITE_PREFIX_MOV_ECX_IMM32{ SITE_PREFIX_MOV_ECX_IMM32_BYTES,
                                                                   sizeof( SITE_PREFIX_MOV_ECX_IMM32_BYTES ) };

// A site with no instruction around its value. Spelled out rather than defaulted, so a call site says which it is.
inline constexpr SSiteInstructionPrefix SITE_PREFIX_NONE{ nullptr, 0 };

static_assert( SITE_PREFIX_MOV_EAX_IMM32.m_ucLength == 2,
               "C7 with a register indirect ModRM is two bytes before the operand, so a guard assuming one byte would "
               "test the ModRM and report every one of these sites as changed" );
static_assert( SITE_PREFIX_MOV_EAX_DISP8_IMM32.m_ucLength == 3,
               "C7 with an eight bit displacement is three bytes before the operand" );
static_assert( SITE_PREFIX_PUSH_IMM32.m_ucLength == 1 && SITE_PREFIX_MOV_ECX_IMM32.m_ucLength == 1,
               "the push and the mov ecx forms are both one byte of opcode directly before the operand" );

// In 32 bits a rel32 branch reaches every address the process can hold, so the wrapped difference names no wrong place.
static_assert( sizeof( UINT_PTR ) == sizeof( uint32_t ),
               "the wrapped displacement below reaches every target only while an address is the width of the operand" );

inline uint32_t EncodeRel32( UINT_PTR address, void *func )
{
    const uint32_t next = static_cast<uint32_t>( address ) + REL32_BRANCH_LENGTH;
    return static_cast<uint32_t>( reinterpret_cast<UINT_PTR>( func ) ) - next;
}
inline uint32_t ReadRel32Displacement( UINT_PTR address )
{
    uint32_t displacement;
    static_assert( static_cast<int>( sizeof( displacement ) ) == REL32_DISPLACEMENT_LENGTH,
                   "every caller proves REL32_BRANCH_LENGTH readable before this reads, so the operand copied here has "
                   "to be exactly the length that guard covers past the opcode" );
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

// The module that displaced a taken site, read every frame long after install.
constexpr size_t SITE_OWNER_NAME_LENGTH = 96;

// Leaf name of the module owning the page an address sits in, so a displaced site names the tool that displaced it
// instead of reporting an address nobody can act on.
inline void DescribeAddressOwner( UINT_PTR ullValue, char *pOut, size_t uiOutSize )
{
    if ( pOut == nullptr || uiOutSize == 0 )
        return;
    pOut[0] = '\0';
    if ( ullValue == 0 )
    {
        _snprintf_s( pOut, uiOutSize, _TRUNCATE, "no module, because the site held nothing" );
        return;
    }
    MEMORY_BASIC_INFORMATION region{};
    if ( VirtualQuery( reinterpret_cast<const void *>( ullValue ), &region, sizeof( region ) ) == 0 ||
         region.AllocationBase == nullptr )
    {
        _snprintf_s( pOut, uiOutSize, _TRUNCATE, "no loaded module" );
        return;
    }
    char szModulePath[MAX_PATH]{};
    if ( GetModuleFileNameA( static_cast<HMODULE>( region.AllocationBase ), szModulePath, sizeof( szModulePath ) ) == 0 )
    {
        _snprintf_s( pOut, uiOutSize, _TRUNCATE, "no loaded module" );
        return;
    }
    // A path that doesnt fit is cut at the buffer size with no terminator written, so the last byte is set here: the
    // scan and the copy below both stop at a zero byte.
    szModulePath[sizeof( szModulePath ) - 1] = '\0';
    const char *pLeaf = szModulePath;
    for ( const char *pScan = szModulePath; *pScan != '\0'; ++pScan )
    {
        if ( *pScan == '\\' || *pScan == '/' )
            pLeaf = pScan + 1;
    }
    _snprintf_s( pOut, uiOutSize, _TRUNCATE, "%s", pLeaf );
}

constexpr unsigned long MAX_HELD_CODE_SITES = 48;

// Re-takes allowed per site before standing down. Refusals spend one each, which bounds the log.
constexpr unsigned long MAX_CODE_SITE_RETAKES = 3;

struct SHeldCodeSite
{
    const char *m_pName;
    UINT_PTR m_ullSite;
    void *m_pFunc;
    unsigned char m_ucOpcode;
    bool m_bSitePinned;
    std::atomic<unsigned long> m_ulStateReported;
    // Only the atomics cross threads. The rest settle before the count publishes the row. Chain target is where frames
    // go when another module held the entry, zero for none.
    std::atomic<UINT_PTR> m_ullChainTarget;
    unsigned long m_ulRetakeAttempts;
    // Evidence arming was true: a re-take while this is zero drops the other module's hook.
    std::atomic<unsigned long> m_ulCompletedHandOvers;
    std::atomic<bool> m_bChainsFrame;
    bool m_bContested;
};

// Four outcomes, because an unreadable site, a site that is no longer a branch, a branch going elsewhere and our own
// branch are four different states.
enum class ECodeSiteState
{
    Unreadable,
    NotOurOpcode,
    HoldsOtherBranch,
    HoldsOurBranch
};

constexpr unsigned long CODE_SITE_REPORTED_UNREADABLE = 1UL << 0;
constexpr unsigned long CODE_SITE_REPORTED_NOT_OURS = 1UL << 1;
constexpr unsigned long CODE_SITE_REPORTED_OTHER_BRANCH = 1UL << 2;

inline std::atomic<unsigned long> &HeldCodeSiteCount()
{
    static std::atomic<unsigned long> s_ulCount{ 0 };
    return s_ulCount;
}

inline SHeldCodeSite *HeldCodeSites()
{
    static SHeldCodeSite s_aSites[MAX_HELD_CODE_SITES]{};
    return s_aSites;
}

// Registered by site, so a second pass updates its row. The count publishes last; a full table returns MAX_HELD_CODE_SITES.
inline unsigned long RegisterHeldCodeSite( const char *pName, UINT_PTR ullSite, void *pFunc, unsigned char ucOpcode )
{
    std::atomic<unsigned long> &ulCount = HeldCodeSiteCount();
    SHeldCodeSite *pSites = HeldCodeSites();
    const unsigned long ulRegistered = ulCount.load( std::memory_order_acquire );
    SHeldCodeSite *pRow = nullptr;
    for ( unsigned long ulIndex = 0; ulIndex < ulRegistered; ++ulIndex )
    {
        if ( pSites[ulIndex].m_ullSite == ullSite )
        {
            pRow = &pSites[ulIndex];
            break;
        }
    }
    unsigned long ulNewIndex = MAX_HELD_CODE_SITES;
    if ( pRow == nullptr )
    {
        if ( ulRegistered >= MAX_HELD_CODE_SITES )
        {
            // A refusal and not a note: this is a hook we wrote and can never check again, which leaves a frame broken
            // with nothing in the log to say why.
            ReportPatchFailure( pName, reinterpret_cast<void *>( ullSite ),
                                "has no room left in the table of code hooks this plugin checks every frame, so this "
                                "hook is installed and never checked again" );
            return MAX_HELD_CODE_SITES;
        }
        pRow = &pSites[ulRegistered];
        pRow->m_ullSite = ullSite;
        ulNewIndex = ulRegistered;
    }
    else if ( pRow->m_pName == pName && pRow->m_pFunc == pFunc && pRow->m_ucOpcode == ucOpcode )
    {
        // A repeat registration leaves the row alone: the fields rewritten below are what a frame check reads, and the
        // cleared state bits are what keep one fault from being reported on every later frame.
        return static_cast<unsigned long>( pRow - pSites );
    }
    pRow->m_pName = pName;
    pRow->m_pFunc = pFunc;
    pRow->m_ucOpcode = ucOpcode;
    // Proved once here and recorded on the row, so the frame check may read the branch without asking the kernel: five
    // bytes proved inside our own image cant be unmapped under a hook running out of it.
    pRow->m_bSitePinned = IsInHostImage( reinterpret_cast<const void *>( ullSite ), REL32_BRANCH_LENGTH );
    pRow->m_ulStateReported.store( 0, std::memory_order_release );
    if ( ulNewIndex != MAX_HELD_CODE_SITES )
    {
        // Only a new row starts the budget. One registered again keeps what it spent, or an install would reset it forever.
        pRow->m_ullChainTarget = 0;
        pRow->m_ulRetakeAttempts = 0;
        pRow->m_ulCompletedHandOvers = 0;
        pRow->m_bChainsFrame = false;
        pRow->m_bContested = false;
        ulCount.store( ulNewIndex + 1, std::memory_order_release );
        return ulNewIndex;
    }
    return static_cast<unsigned long>( pRow - pSites );
}

// Arms a site and publishes the branch its frames go to. Only a handler whose prologue saves no callee saved register
// may arm, which is a per site fact from disassembly and why nothing arms today. A zero target means nothing displaced.
inline void ArmHeldCodeSiteForChaining( unsigned long ulRow, UINT_PTR ullChainTarget )
{
    const unsigned long ulCount = HeldCodeSiteCount().load( std::memory_order_acquire );
    if ( ulRow >= ulCount )
        return;
    SHeldCodeSite *pSites = HeldCodeSites();
    if ( ullChainTarget != 0 )
        pSites[ulRow].m_ullChainTarget.store( ullChainTarget, std::memory_order_release );
    pSites[ulRow].m_bChainsFrame.store( true, std::memory_order_release );
}

inline UINT_PTR HeldCodeSiteChainTarget( unsigned long ulRow )
{
    const unsigned long ulCount = HeldCodeSiteCount().load( std::memory_order_acquire );
    if ( ulRow >= ulCount )
        return 0;
    return HeldCodeSites()[ulRow].m_ullChainTarget.load( std::memory_order_acquire );
}

inline void NoteHeldCodeSiteHandOver( unsigned long ulRow )
{
    const unsigned long ulCount = HeldCodeSiteCount().load( std::memory_order_acquire );
    if ( ulRow >= ulCount )
        return;
    HeldCodeSites()[ulRow].m_ulCompletedHandOvers.fetch_add( 1, std::memory_order_acq_rel );
}

// Reads the branch a registered site holds now and says which of the four states it is in. A pinned site copies its
// bytes directly, since registration proved the whole branch is inside our own image; anything else is read carefully.
// The target is produced for the two states that have one, so a report can name where the branch goes.
inline ECodeSiteState ClassifyHeldCodeSite( const SHeldCodeSite &site, UINT_PTR &ullTarget )
{
    ullTarget = 0;
    if ( !site.m_bSitePinned &&
         !IsReadableRange( reinterpret_cast<const void *>( site.m_ullSite ), REL32_BRANCH_LENGTH ) )
        return ECodeSiteState::Unreadable;
    if ( *reinterpret_cast<const unsigned char *>( site.m_ullSite ) != site.m_ucOpcode )
        return ECodeSiteState::NotOurOpcode;
    ullTarget = site.m_ullSite + REL32_BRANCH_LENGTH + ReadRel32Displacement( site.m_ullSite );
    return ullTarget == reinterpret_cast<UINT_PTR>( site.m_pFunc ) ? ECodeSiteState::HoldsOurBranch
                                                                  : ECodeSiteState::HoldsOtherBranch;
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
    // Read access is not execute access, and only this test covers the bytes being code.
    if ( !IsExecutableRange( reinterpret_cast<const void *>( address ), REL32_BRANCH_LENGTH ) )
    {
        ReportPatchFailure( "InstallCall", reinterpret_cast<void *>( address ),
                            "is not memory this process can execute, so no call was written there" );
        return EWriteResult::Failed;
    }
    // A call cant be chained, so another module's call here is taken over rather than refused: refusing loses our hook and
    // leaving theirs loses the hook this site exists for. A call into our own image is the game's own, which says nothing.
    const UINT_PTR ullExistingCall = address + REL32_BRANCH_LENGTH + ReadRel32Displacement( address );
    // Built first, reported once the write landed: a refused write leaves that module's call where it was.
    char szOwner[SITE_OWNER_NAME_LENGTH];
    char szNote[320];
    bool bDisplacedCallWorthReporting = false;
    if ( ullExistingCall != reinterpret_cast<UINT_PTR>( func ) )
    {
        // The same three answers the value sites give, because the two cases where we cant tell which module put a value
        // there have to say exactly that. Naming a module we never identified would send a maintainer after a tool that
        // was never there.
        switch ( ClassifyHostImage( reinterpret_cast<const void *>( ullExistingCall ), REL32_BRANCH_LENGTH ) )
        {
        case EHostImage::Contained:
            break;
        case EHostImage::Outside:
            DescribeAddressOwner( ullExistingCall, szOwner, sizeof( szOwner ) );
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "already held a call to 0x%08X in %s before this plugin replaced it, so that module no longer "
                         "sees this call; a call carries no chain, so the site was taken rather than left",
                         static_cast<unsigned int>( ullExistingCall ), szOwner );
            bDisplacedCallWorthReporting = true;
            break;
        case EHostImage::Relocated:
        case EHostImage::Unknown:
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "already held a call to 0x%08X and this plugin cannot say which module put it there, because "
                         "the executable's own image could not be read, so the site was taken and that caller's call "
                         "is not recorded",
                         static_cast<unsigned int>( ullExistingCall ) );
            bDisplacedCallWorthReporting = true;
            break;
        }
    }
    // One write for all five bytes; split, the site would hold a new opcode beside the old operand.
    const uint32_t rel = EncodeRel32( address, func );
    unsigned char call[REL32_BRANCH_LENGTH];
    static_assert( REL32_OPCODE_LENGTH + static_cast<int>( sizeof( rel ) ) == REL32_BRANCH_LENGTH,
                   "the operand copy starts after the opcode and has to end exactly at the end of this buffer" );
    call[0] = REL32_CALL_OPCODE;
    memcpy( call + REL32_OPCODE_LENGTH, &rel, sizeof( rel ) );
    // WriteProtected directly, so an unseen write isnt flattened to a success.
    const EWriteResult result =
        WriteProtected( "InstallCall", reinterpret_cast<void *>( address ), sizeof( call ),
                        [&call] ( void *destination ) { memcpy( destination, call, sizeof( call ) ); } );
    // Reported only for a write that landed: a refused write displaced nothing.
    if ( result != EWriteResult::Failed && bDisplacedCallWorthReporting )
        ReportPatchNote( "InstallCall", szNote );
    // Registered only for a write that landed, so a refused install leaves no row the frame check spends every frame on.
    if ( result != EWriteResult::Failed )
        RegisterHeldCodeSite( "InstallCall", address, func, REL32_CALL_OPCODE );
    return result;
}

// bReport false only for the re-take: a success line there would read as a fresh install.
inline EWriteResult InstallJump( UINT_PTR address, void *func, UINT_PTR *pChainTarget = nullptr,
                                 unsigned long *pSiteRow = nullptr, bool bReportWrite = true,
                                 SWriteRefusal *pRefusal = nullptr, int iExpectedOpcode = -1 )
{
    // Tested before the site is read for a jump, so one site reaches one verdict.
    if ( !IsExecutableRange( reinterpret_cast<const void *>( address ), REL32_BRANCH_LENGTH ) )
    {
        ReportPatchFailure( "RedirectJump", reinterpret_cast<void *>( address ),
                            "is not memory this process can execute, so no jump was written there" );
        return EWriteResult::Failed;
    }
    // A caller naming the opcode overwrites another module's instruction: five bytes over a different framing splits it.
    if ( iExpectedOpcode >= 0 &&
         ( !IsReadableRange( reinterpret_cast<const void *>( address ), REL32_OPCODE_LENGTH ) ||
           *reinterpret_cast<const unsigned char *>( address ) != static_cast<unsigned char>( iExpectedOpcode ) ) )
    {
        ReportPatchFailure( "RedirectJump", reinterpret_cast<void *>( address ),
                            "no longer holds the jump this plugin wrote, so it was left as it stands rather than "
                            "overwritten on a guess at how another module framed its instruction" );
        return EWriteResult::Failed;
    }
    // Kept until the write lands, so a failed write never leaves the target naming a hook we didnt displace.
    UINT_PTR ullChainTarget = 0;
    // Reported only once the write landed: a refused write displaced nothing.
    char szNote[320];
    bool bDisplacedJumpWorthReporting = false;
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
            // Registered here too: the site holds our branch, which is what the frame check watches, and leaving it
            // unregistered would make a second pass stop watching every site the first one installed.
            const unsigned long ulHeldRow = RegisterHeldCodeSite( "RedirectJump", address, func, REL32_JUMP_OPCODE );
            if ( pSiteRow != nullptr )
                *pSiteRow = ulHeldRow;
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
            // Taken because this caller cant run a displaced hook, and named for the reason InstallCall names a displaced
            // call: the code this jump reached is no longer reached, and this is its only record.
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "already held a jump to 0x%08X inside this executable's own image, which cannot be chained, "
                         "so the code that jump reached is no longer reached from this site",
                         static_cast<unsigned int>( existingTarget ) );
            bDisplacedJumpWorthReporting = true;
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
                   "the operand copy starts after the opcode and has to end exactly at the end of this buffer" );
    jump[0] = REL32_JUMP_OPCODE;
    memcpy( jump + REL32_OPCODE_LENGTH, &rel, sizeof( rel ) );
    const EWriteResult result = WriteProtected( "RedirectJump", reinterpret_cast<void *>( address ), sizeof( jump ),
                                                [&jump] ( void *destination ) { memcpy( destination, jump, sizeof( jump ) ); },
                                                bReportWrite, pRefusal );
    // Published only for a write that landed, and only to a caller that asked.
    if ( result != EWriteResult::Failed && pChainTarget != nullptr )
        *pChainTarget = ullChainTarget;
    // Reported only for a write that landed, for the reason above.
    if ( result != EWriteResult::Failed && bDisplacedJumpWorthReporting )
        ReportPatchNote( "RedirectJump", szNote );
    if ( result != EWriteResult::Failed )
    {
        const unsigned long ulHeldRow = RegisterHeldCodeSite( "RedirectJump", address, func, REL32_JUMP_OPCODE );
        if ( pSiteRow != nullptr )
            *pSiteRow = ulHeldRow;
    }
    return result;
}

inline EWriteResult RedirectJump( UINT_PTR address, void *func, UINT_PTR *pChainTarget = nullptr,
                                  unsigned long *pSiteRow = nullptr, bool bReportWrite = true,
                                  SWriteRefusal *pRefusal = nullptr, int iExpectedOpcode = -1 )
{
    return InstallJump( address, func, pChainTarget, pSiteRow, bReportWrite, pRefusal, iExpectedOpcode );
}

// Fixed sites in this process are written by more than this plugin. The code hooks above classify what a site holds and
// refuse to take another module's jump, because a jump can be chained. A data site holds a value and nothing can be
// chained through it, so the two cases below classify first and then act rather than writing blind.

// One row per site we own. Bounded, because the set is fixed at compile time and a table that grows with runtime input
// would be the defect it is meant to detect.
constexpr unsigned long MAX_EXCLUSIVE_SLOTS = 32;

// Two kinds, because they answer different questions. HoldsOurValue is a callback or render table entry: anything but our
// own value stops the frame drawing, so the value is written back. AtLeastOurValue is a capacity handed to an allocator:
// another module may raise it for this same game, so a higher value is healthy and a raise we made is never undone.
enum class ESiteKind
{
    HoldsOurValue,
    AtLeastOurValue
};

struct SExclusiveSlot
{
    const char *m_pName;
    UINT_PTR m_ullSite;
    UINT_PTR m_ullOurValue;
    int m_iFloor;
    SSiteInstructionPrefix m_expectedPrefix;
    ESiteKind m_eKind;
    unsigned long m_ulDisplacements;
    bool m_bSitePinned;
    bool m_bFirstReported;
    bool m_bRepeatReported;
    // One bit per restore outcome, since a contended slot is transient and one flag would silence a later cause.
    unsigned long m_ulRestoreReported;
    // One bit per state, because an unreadable site and a replaced instruction are acted on differently.
    bool m_bUnreadableReported;
    unsigned long m_ulInstructionReported;
    bool m_bLoweredReported;
    bool m_bLoweredRepeatReported;
};

inline std::atomic<unsigned long> &ExclusiveSlotCount()
{
    static std::atomic<unsigned long> s_ulCount{ 0 };
    return s_ulCount;
}

// One instance for the whole process, however many translation units include this header.
inline SExclusiveSlot *ExclusiveSlots()
{
    static SExclusiveSlot s_aSlots[MAX_EXCLUSIVE_SLOTS]{};
    return s_aSlots;
}

inline bool ReadSitePointer( UINT_PTR ullSite, UINT_PTR &ullValue )
{
    ullValue = 0;
    if ( !IsReadableRange( reinterpret_cast<const void *>( ullSite ), sizeof( ullValue ) ) )
        return false;
    memcpy( &ullValue, reinterpret_cast<const void *>( ullSite ), sizeof( ullValue ) );
    return true;
}

// The frame check reads a registered site without asking the kernel first, which is sound only for a site proved inside
// our own image at registration: the image cant unmap under a hook running out of it. Asking costs two memory queries
// per site per frame for an answer that cant change.
inline UINT_PTR ReadPinnedSite( const SExclusiveSlot &slot )
{
    UINT_PTR ullValue = 0;
    memcpy( &ullValue, reinterpret_cast<const void *>( slot.m_ullSite ), sizeof( ullValue ) );
    return ullValue;
}

inline int ReadPinnedLimit( const SExclusiveSlot &slot )
{
    int iValue = 0;
    memcpy( &iValue, reinterpret_cast<const void *>( slot.m_ullSite ), sizeof( iValue ) );
    return iValue;
}

// The careful counterpart of ReadPinnedLimit, for a row whose address was not proved inside our own image. RaiseIntLimit
// publishes such a row rather than refusing to watch it. Two memory queries a frame against a limit nobody correcting: a
// limit another module lowers is what turns the streaming hook into entities that keep their place and lose their drawing.
inline bool ReadSiteLimit( UINT_PTR ullSite, int &iValue )
{
    iValue = 0;
    if ( !IsReadableRange( reinterpret_cast<const void *>( ullSite ), sizeof( iValue ) ) )
        return false;
    memcpy( &iValue, reinterpret_cast<const void *>( ullSite ), sizeof( iValue ) );
    return true;
}

inline SExclusiveSlot *FindExclusiveSlot( UINT_PTR ullSite )
{
    const unsigned long ulCount = ExclusiveSlotCount().load( std::memory_order_acquire );
    SExclusiveSlot *pSlots = ExclusiveSlots();
    for ( unsigned long ulIndex = 0; ulIndex < ulCount; ++ulIndex )
    {
        if ( pSlots[ulIndex].m_ullSite == ullSite )
            return &pSlots[ulIndex];
    }
    return nullptr;
}

// Registered by site, so a second pass updates its row rather than double counting the audit. The count is published
// last, because a frame check reading a row mid-registration would see a site of zero and report it as unreadable.
inline SExclusiveSlot *PublishExclusiveSlot( const char *pName, UINT_PTR ullSite, UINT_PTR ullOurValue, int iFloor,
                                             SSiteInstructionPrefix expectedPrefix, ESiteKind eKind, bool bPinned,
                                             bool bReport = true )
{
    std::atomic<unsigned long> &ulCount = ExclusiveSlotCount();
    SExclusiveSlot *pSlot = FindExclusiveSlot( ullSite );
    unsigned long ulNewIndex = MAX_EXCLUSIVE_SLOTS;
    if ( pSlot == nullptr )
    {
        const unsigned long ulIndex = ulCount.load( std::memory_order_relaxed );
        if ( ulIndex >= MAX_EXCLUSIVE_SLOTS )
        {
            // This wording may only say the site stops being watched: RaiseIntLimit registers a limit it found already
            // at or above its floor without writing, so naming a write here would claim one this branch cant know
            // happened. A fault, not a note, because a site with no row is never checked again.
            if ( bReport )
                ReportPatchFailure( pName, reinterpret_cast<void *>( ullSite ),
                                    "has no room left in the table of sites this plugin keeps its own value in, so "
                                    "this site is not watched after this call and will not be for the rest of this run" );
            return nullptr;
        }
        pSlot = &ExclusiveSlots()[ulIndex];
        pSlot->m_ullSite = ullSite;
        ulNewIndex = ulIndex;
    }
    else if ( pSlot->m_pName == pName && pSlot->m_ullOurValue == ullOurValue && pSlot->m_iFloor == iFloor &&
              pSlot->m_eKind == eKind && pSlot->m_bSitePinned == bPinned &&
              pSlot->m_expectedPrefix.m_pBytes == expectedPrefix.m_pBytes &&
              pSlot->m_expectedPrefix.m_ucLength == expectedPrefix.m_ucLength )
    {
        // A row already describing this registration is left as it stands. Two of the three callers dont hold the frame
        // check's slot: the reassert hook runs on the init path and the streaming add runs on the streaming thread, both
        // repeating with the same arguments. Writing the row from either would race the frame check and the site watch.
        return pSlot;
    }
    pSlot->m_pName = pName;
    pSlot->m_ullOurValue = ullOurValue;
    pSlot->m_iFloor = iFloor;
    pSlot->m_expectedPrefix = expectedPrefix;
    pSlot->m_eKind = eKind;
    pSlot->m_bSitePinned = bPinned;
    pSlot->m_ulDisplacements = 0;
    pSlot->m_bFirstReported = false;
    pSlot->m_bRepeatReported = false;
    pSlot->m_ulRestoreReported = 0;
    pSlot->m_bUnreadableReported = false;
    pSlot->m_ulInstructionReported = 0;
    pSlot->m_bLoweredReported = false;
    pSlot->m_bLoweredRepeatReported = false;
    if ( ulNewIndex != MAX_EXCLUSIVE_SLOTS )
        ulCount.store( ulNewIndex + 1, std::memory_order_release );
    return pSlot;
}

// A site we must own: a pipeline callback, or a render table entry the renderer reaches its own code through. Already
// holding our value: nothing is written, so a second pass and an untouched image behave the same. Holding another
// module's value: named, then replaced, because a geometry callback that doesnt reach this renderer stops the world drawing.
inline EWriteResult SetPointerExclusive( const char *pOperation, UINT_PTR ullSite, void *pOurValue,
                                         SSiteInstructionPrefix expectedPrefix = SITE_PREFIX_NONE )
{
    const UINT_PTR ullOurs = reinterpret_cast<UINT_PTR>( pOurValue );
    UINT_PTR ullCurrent = 0;
    if ( !ReadSitePointer( ullSite, ullCurrent ) )
    {
        ReportPatchFailure( pOperation, reinterpret_cast<void *>( ullSite ),
                            "does not hold readable memory, so what it holds cannot be named and it was left alone" );
        return EWriteResult::Failed;
    }
    // Unconditional, because the subtraction below always runs: a prefix with a length but no bytes would reach it with
    // nothing having rejected it.
    if ( !IsSitePrefixWellFormed( expectedPrefix ) )
    {
        ReportPatchFailure( pOperation, reinterpret_cast<void *>( ullSite ),
                            "was given an instruction prefix whose bytes and length disagree about whether there is "
                            "one, so nothing was written there" );
        return EWriteResult::Failed;
    }
    if ( ullSite < expectedPrefix.m_ucLength )
    {
        ReportPatchFailure( pOperation, reinterpret_cast<void *>( ullSite ),
                            "is too close to the start of the address space for the instruction this hook was built "
                            "against to be in front of it, so it was left alone" );
        return EWriteResult::Failed;
    }
    // Proved once here and recorded on the row, so the frame check may read the site without a memory query. The proof
    // covers the instruction in front of the value too: stopping at the value would leave the prefix read unproved.
    const UINT_PTR ullProvenFrom = ullSite - expectedPrefix.m_ucLength;
    const SIZE_T uiProvenSize = static_cast<SIZE_T>( expectedPrefix.m_ucLength ) + sizeof( ullCurrent );
    // Tested at install as well as in the frame check, and here first: this is the earlier write, and a frame check cant
    // put back a site already written into an instruction it was never checked against.
    if ( HasSitePrefix( expectedPrefix ) )
    {
        const EBytesMatch prefixMatch =
            CompareExpectedBytes( ullProvenFrom, expectedPrefix.m_pBytes, expectedPrefix.m_ucLength );
        if ( prefixMatch != EBytesMatch::Match )
        {
            ReportPatchFailure( pOperation, reinterpret_cast<void *>( ullSite ),
                                prefixMatch == EBytesMatch::Unreadable
                                    ? "does not sit behind readable memory, so the instruction this hook was built "
                                      "against cannot be read and nothing was written there"
                                    : "does not sit behind the instruction this hook was built against, so nothing "
                                      "was written there rather than write into code this plugin was never checked "
                                      "against" );
            return EWriteResult::Failed;
        }
    }
    const bool bPinned = IsInHostImage( reinterpret_cast<const void *>( ullProvenFrom ), uiProvenSize );
    EWriteResult result = EWriteResult::Clean;
    if ( ullCurrent != ullOurs )
    {
        char szNote[320];
        bool bNoteWorthReporting = false;
        if ( ullCurrent == 0 )
        {
            // A site holding nothing is a table entry no writer filled yet, not one another module took, and naming a
            // writer that was never there sends a maintainer looking for a tool that doesnt exist.
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "held nothing before this plugin wrote its own value, so no other writer had claimed it" );
            bNoteWorthReporting = true;
        }
        else
        {
            // A healthy install finds the executable's own value here: the callback it pushed for itself, or the pointer it
            // put in its own table. Reporting that would report a conflict on every clean load, so only a value from outside
            // our image counts as a writer that displaced the site.
            char szOwner[SITE_OWNER_NAME_LENGTH];
            switch ( ClassifyHostImage( reinterpret_cast<const void *>( ullCurrent ), sizeof( ullCurrent ) ) )
            {
            case EHostImage::Contained:
                // The executable's own value, which is what we are here to replace. The install is reported once already,
                // so a second line would be the load reporting its own normal work.
                szNote[0] = '\0';
                break;
            case EHostImage::Outside:
                DescribeAddressOwner( ullCurrent, szOwner, sizeof( szOwner ) );
                _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                             "held 0x%08X from %s before this plugin wrote its own value, so another writer had this "
                             "site first",
                             static_cast<unsigned int>( ullCurrent ), szOwner );
                bNoteWorthReporting = true;
                break;
            case EHostImage::Relocated:
                _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                             "held 0x%08X and this plugin cannot say which module put it there, because the "
                             "executable is not loaded at the address it was linked at, so a value from the "
                             "executable itself and a value from another module read the same here",
                             static_cast<unsigned int>( ullCurrent ) );
                bNoteWorthReporting = true;
                break;
            case EHostImage::Unknown:
                _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                             "held 0x%08X and this plugin cannot say which module put it there, because it could not "
                             "read the executable's own image, so a value from the executable itself and a value from "
                             "another module read the same here",
                             static_cast<unsigned int>( ullCurrent ) );
                bNoteWorthReporting = true;
                break;
            }
        }
        result = SetPointer( ullSite, pOurValue, pOperation );
        // Reported only for a write that landed: a refused write replaced nothing.
        if ( result != EWriteResult::Failed && bNoteWorthReporting )
            ReportPatchNote( pOperation, szNote );
    }
    // Registered only once the site really holds our value. A refused write that registered anyway would leave a row the
    // frame check spends every frame trying to take back from a site it never took, and the audit would read it as ours.
    if ( result == EWriteResult::Failed )
        return result;
    PublishExclusiveSlot( pOperation, ullSite, ullOurs, 0, expectedPrefix, ESiteKind::HoldsOurValue, bPinned );
    return result;
}

// A capacity handed to an allocator as a four byte constant inside one instruction, and only ever raised: the streaming
// hook deletes the rw object of any entity it cant fit, so a lower limit is a world that keeps its collision and stops
// drawing. The opcode is tested first, since four bytes written into an unchecked instruction leave broken code rather
// than a wrong number. bReport is false on the streaming path, where the logger holds its mutex across a file write.
inline EWriteResult RaiseIntLimit( const char *pOperation, UINT_PTR ullSite, int iFloor,
                                   SSiteInstructionPrefix expectedPrefix, bool bReport = true )
{
    if ( !IsReadableRange( reinterpret_cast<const void *>( ullSite ), sizeof( int ) ) )
    {
        if ( bReport )
            ReportPatchFailure( pOperation, reinterpret_cast<void *>( ullSite ),
                                "does not hold readable memory, so the limit it holds now cannot be read and it was "
                                "left alone" );
        return EWriteResult::Failed;
    }
    if ( !IsSitePrefixWellFormed( expectedPrefix ) || !HasSitePrefix( expectedPrefix ) ||
         ullSite < expectedPrefix.m_ucLength )
    {
        if ( bReport )
            ReportPatchFailure( pOperation, reinterpret_cast<void *>( ullSite ),
                                "was not given the instruction this hook was built against, so it was left alone "
                                "rather than written into code this plugin was never checked against" );
        return EWriteResult::Failed;
    }
    const UINT_PTR ullPrefix = ullSite - expectedPrefix.m_ucLength;
    const EBytesMatch prefixMatch = CompareExpectedBytes( ullPrefix, expectedPrefix.m_pBytes,
                                                          expectedPrefix.m_ucLength );
    if ( prefixMatch == EBytesMatch::Unreadable )
    {
        // Names the instruction, not the limit: the guard above proved the four byte value readable, so a message saying
        // the limit cant be read would contradict what this call established.
        if ( bReport )
            ReportPatchFailure( pOperation, reinterpret_cast<void *>( ullSite ),
                                "does not sit behind readable memory, so the instruction this hook was built against "
                                "cannot be read and the limit was left as it stands" );
        return EWriteResult::Failed;
    }
    if ( prefixMatch == EBytesMatch::Mismatch )
    {
        if ( bReport )
            ReportPatchFailure( pOperation, reinterpret_cast<void *>( ullSite ),
                                "does not sit behind the instruction this hook was built against, so it was left "
                                "alone rather than written into code this plugin was never checked against" );
        return EWriteResult::Failed;
    }
    int iCurrent = 0;
    memcpy( &iCurrent, reinterpret_cast<const void *>( ullSite ), sizeof( iCurrent ) );
    char szNote[256];
    EWriteResult result = EWriteResult::Clean;
    if ( iCurrent >= iFloor )
    {
        if ( bReport )
        {
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "already reads %d, which is at or above this plugin's %d, so it was left as it stands",
                         iCurrent, iFloor );
            ReportPatchNote( pOperation, szNote );
        }
    }
    else
    {
        result = SetInt( ullSite, iFloor, pOperation, bReport );
        if ( result == EWriteResult::Failed )
            return result;
        if ( bReport )
        {
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE, "read %d and was raised to this plugin's %d", iCurrent,
                         iFloor );
            ReportPatchNote( pOperation, szNote );
        }
    }
    // Registered whether or not the proof succeeded: it sits on the row rather than gating anything. An unproved row is
    // read through ReadSiteLimit, which asks the system first, so it costs two queries a frame instead of going unwatched;
    // a note, not a fault, since the site is watched either way. The proof covers the instruction in front of the value too.
    const bool bPinned = IsInHostImage( reinterpret_cast<const void *>( ullPrefix ),
                                        static_cast<SIZE_T>( expectedPrefix.m_ucLength ) + sizeof( int ) );
    if ( !bPinned && bReport )
        ReportPatchNote( pOperation,
                         "is watched the slower way, because this plugin could not confirm that the address it would "
                         "check is this executable's own image at the address it was linked at, so every check asks "
                         "the system about that address before it reads it" );
    PublishExclusiveSlot( pOperation, ullSite, 0, iFloor, expectedPrefix, ESiteKind::AtLeastOurValue, bPinned,
                          bReport );
    return result;
}

// True when the instruction we wrote into is still the one we were built against. Three other states, because an
// instruction that could not be read, one that was replaced and a descriptor describing no instruction are different
// things to act on. A value in the executable's data has no instruction around it and is always intact.
enum class ESiteInstructionState
{
    Intact,
    Unreadable,
    Replaced,
    Unverifiable
};

inline ESiteInstructionState ClassifySiteInstruction( const SExclusiveSlot &slot )
{
    // A descriptor whose two members disagree describes no instruction, so it can never be shown intact. Fails closed,
    // because the alternative is writing into a site without having tested anything.
    if ( !IsSitePrefixWellFormed( slot.m_expectedPrefix ) )
        return ESiteInstructionState::Unverifiable;
    if ( !HasSitePrefix( slot.m_expectedPrefix ) )
        return ESiteInstructionState::Intact;
    // Fails closed here too, because this subtraction is the one place the function could address memory it never proved:
    // a site nearer the start of the address space than its instruction would wrap and read as one that passed.
    if ( slot.m_ullSite < slot.m_expectedPrefix.m_ucLength )
        return ESiteInstructionState::Unverifiable;
    const UINT_PTR ullPrefix = slot.m_ullSite - slot.m_expectedPrefix.m_ucLength;
    if ( slot.m_bSitePinned )
    {
        // Registration proved every byte of the prefix inside our own image, so this read cant fail and a difference is
        // a replaced instruction rather than an unreadable one.
        return memcmp( reinterpret_cast<const void *>( ullPrefix ), slot.m_expectedPrefix.m_pBytes,
                       slot.m_expectedPrefix.m_ucLength ) == 0
                   ? ESiteInstructionState::Intact
                   : ESiteInstructionState::Replaced;
    }
    switch ( CompareExpectedBytes( ullPrefix, slot.m_expectedPrefix.m_pBytes, slot.m_expectedPrefix.m_ucLength ) )
    {
    case EBytesMatch::Match:
        return ESiteInstructionState::Intact;
    case EBytesMatch::Unreadable:
        return ESiteInstructionState::Unreadable;
    case EBytesMatch::Mismatch:
        break;
    }
    return ESiteInstructionState::Replaced;
}

// The write a frame makes when another module has taken a site we own. Quiet, for the reason WriteProtected's last
// parameter gives, and the caller reports every outcome other than a clean write once instead.
inline EWriteResult PatchHeldSite( const char *pOperation, UINT_PTR ullSite, const void *pData, SIZE_T uiSize,
                                   SWriteRefusal &refusal )
{
    return WriteProtected( pOperation, reinterpret_cast<void *>( ullSite ), uiSize,
                           [pData, uiSize] ( void *destination ) { memcpy( destination, pData, uiSize ); }, false,
                           &refusal );
}

// A quiet write suppresses WriteProtected's own reports, so every outcome other than a clean write is owed one here. The
// reason travels with the outcome: another thread holding the slot isnt the same failure as a page that couldnt be made
// writable, and a write that landed without its protection back is not a success.
inline void ReportRestoreOutcomeOnce( SExclusiveSlot &slot, EWriteResult result, const SWriteRefusal &refusal )
{
    // One bit per outcome, so each state reports once and only repetition is bounded.
    enum : unsigned long
    {
        REPORTED_REFUSED_NOT_ONE_REGION       = 1UL << 0,
        REPORTED_REFUSED_SLOT_CONTENDED       = 1UL << 1,
        REPORTED_REFUSED_NOT_WRITABLE         = 1UL << 2,
        REPORTED_REFUSED_UNEXPLAINED          = 1UL << 3,
        REPORTED_UNCLEAN_PROTECTION_AND_FLUSH = 1UL << 4,
        REPORTED_UNCLEAN_PROTECTION           = 1UL << 5,
        REPORTED_UNCLEAN_FLUSH                = 1UL << 6,
        REPORTED_UNCLEAN_UNEXPLAINED          = 1UL << 7
    };
    if ( result == EWriteResult::Clean )
        return;
    unsigned long ulOutcome = REPORTED_UNCLEAN_UNEXPLAINED;
    if ( result == EWriteResult::Failed )
    {
        if ( refusal.m_bNotOneRegion )
            ulOutcome = REPORTED_REFUSED_NOT_ONE_REGION;
        else if ( refusal.m_bWriteSlotContended )
            ulOutcome = REPORTED_REFUSED_SLOT_CONTENDED;
        else if ( refusal.m_bNotWritable )
            ulOutcome = REPORTED_REFUSED_NOT_WRITABLE;
        else
            ulOutcome = REPORTED_REFUSED_UNEXPLAINED;
    }
    else if ( refusal.m_bNotRestored && refusal.m_bNotFlushed )
        ulOutcome = REPORTED_UNCLEAN_PROTECTION_AND_FLUSH;
    else if ( refusal.m_bNotRestored )
        ulOutcome = REPORTED_UNCLEAN_PROTECTION;
    else if ( refusal.m_bNotFlushed )
        ulOutcome = REPORTED_UNCLEAN_FLUSH;
    if ( ( slot.m_ulRestoreReported & ulOutcome ) != 0 )
        return;
    if ( result == EWriteResult::Failed )
    {
        const char *pBecause = "and this plugin was not told why";
        if ( refusal.m_bNotOneRegion )
            pBecause = "because the site no longer sits inside one committed region";
        else if ( refusal.m_bWriteSlotContended )
            pBecause = "because another thread held this process's single write slot at the time";
        else if ( refusal.m_bNotWritable )
            pBecause = "because its page could not be made writable";
        char szNote[320];
        _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                     "was changed after this plugin wrote it and could not be written back, %s, so it keeps the value "
                     "that was written over this plugin's until a later check writes it back",
                     pBecause );
        ReportPatchFailure( slot.m_pName, reinterpret_cast<void *>( slot.m_ullSite ), szNote );
        slot.m_ulRestoreReported |= ulOutcome;  // after the report, so one that failed stays reportable
        return;
    }
    const char *pState = "was changed after this plugin wrote it and was written back, but that write did not finish "
                         "cleanly and this plugin was not told which part of it failed";
    if ( refusal.m_bNotRestored && refusal.m_bNotFlushed )
        pState = "was changed after this plugin wrote it and was written back, but the processor was not told those "
                 "bytes changed and the page did not get its own protection back, so it may keep using the copy it "
                 "already had and it keeps the write access this plugin added";
    else if ( refusal.m_bNotRestored )
        pState = "was changed after this plugin wrote it and was written back, but the page did not get its own "
                 "protection back and keeps the write access this plugin added";
    else if ( refusal.m_bNotFlushed )
        pState = "was changed after this plugin wrote it and was written back with its own protection, but the "
                 "processor was not told those bytes changed, so it may keep using the copy it already had";
    ReportPatchWrittenNotClean( slot.m_pName, reinterpret_cast<void *>( slot.m_ullSite ), pState );
    slot.m_ulRestoreReported |= ulOutcome;  // after the report, so one that failed stays reportable
}

// The re-take writes quietly, so the quiet flag also silences the writer's own line for the two unclean outcomes.
inline void ReportReclaimWriteOutcome( const SHeldCodeSite &site, EWriteResult result, const SWriteRefusal &refusal )
{
    if ( result == EWriteResult::Clean )
        return;
    if ( result == EWriteResult::Failed )
    {
        // The writer's own reasons. A refusal InstallJump decided before writing sets none, so this is the general case.
        const char *pBecause = "because the install refused this site rather than write there, and named its reason above this line";
        if ( refusal.m_bNotOneRegion )
            pBecause = "because the site no longer sits inside one committed region";
        else if ( refusal.m_bWriteSlotContended )
            pBecause = "because another thread held this process's single write slot at the time";
        else if ( refusal.m_bNotWritable )
            pBecause = "because its page could not be made writable";
        char szNote[320];
        _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                     "could not be written back over the branch that displaced this plugin, %s, so this plugin's branch is "
                     "not back at this entry",
                     pBecause );
        ReportPatchFailure( site.m_pName, reinterpret_cast<void *>( site.m_ullSite ), szNote );
        return;
    }
    const char *pState = "was written back over the branch that displaced this plugin, but that write did not finish cleanly "
                         "and this plugin was not told which part of it failed";
    if ( refusal.m_bNotRestored && refusal.m_bNotFlushed )
        pState = "was written back over the branch that displaced this plugin, but the processor was not told those bytes "
                 "changed and the page did not get its own protection back, so it may keep using the copy it already had and "
                 "it keeps the write access this plugin added";
    else if ( refusal.m_bNotRestored )
        pState = "was written back over the branch that displaced this plugin, but the page did not get its own protection "
                 "back and keeps the write access this plugin added";
    else if ( refusal.m_bNotFlushed )
        pState = "was written back over the branch that displaced this plugin with its own protection, but the processor "
                 "was not told those bytes changed, so it may keep using the copy it already had";
    ReportPatchWrittenNotClean( site.m_pName, reinterpret_cast<void *>( site.m_ullSite ), pState );
}

// Takes a site back from a module that overwrote our branch, recording where its branch went so the handler hands frames
// to it instead of losing them. Nothing is written unless that target is executable and the site still holds a jump opcode.
inline bool AttemptReclaimCodeSite( SHeldCodeSite &site, UINT_PTR ullOtherTarget )
{
    // Without a handler that hands frames on, taking the site back would drop the other module's hook outright.
    if ( !site.m_bChainsFrame.load( std::memory_order_acquire ) )
        return false;
    if ( site.m_bContested )
        return false;
    // A call row comes back as a jump that doesnt push a return address, so it is excluded. None is armed, so untested.
    if ( site.m_ucOpcode != REL32_JUMP_OPCODE )
        return false;
    const UINT_PTR ullAlreadyChained = site.m_ullChainTarget.load( std::memory_order_acquire );
    if ( ullAlreadyChained != 0 && ullAlreadyChained == ullOtherTarget )
        return false;  // already goes where that module sends the entry; the caller reports the site as not held
    if ( site.m_ulRetakeAttempts >= MAX_CODE_SITE_RETAKES )
    {
        site.m_bContested = true;
        const unsigned long ulHandedOver = site.m_ulCompletedHandOvers.load( std::memory_order_acquire );
        char szOwner[SITE_OWNER_NAME_LENGTH];
        DescribeAddressOwner( ullOtherTarget, szOwner, sizeof( szOwner ) );
        char szNote[448];
        _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                     "has been attempted %lu times, and an attempt is spent whether or not its write landed, so this "
                     "plugin stops competing for this site; the branch standing there now is one to 0x%08X in %s and the "
                     "code it replaced runs here instead of its hook from now on; %lu frames had been handed over at "
                     "this site by then",
                     site.m_ulRetakeAttempts, static_cast<unsigned int>( ullOtherTarget ), szOwner, ulHandedOver );
        ReportPatchFailure( site.m_pName, reinterpret_cast<void *>( site.m_ullSite ), szNote );
        site.m_ulStateReported.fetch_or( CODE_SITE_REPORTED_OTHER_BRANCH, std::memory_order_acq_rel );  // after the report, so one that failed stays reportable
        return false;
    }
    if ( !IsExecutableRange( reinterpret_cast<const void *>( ullOtherTarget ), REL32_OPCODE_LENGTH ) )
        return false;
    ++site.m_ulRetakeAttempts;
    UINT_PTR ullChain = 0;
    SWriteRefusal reclaimRefusal;
    const EWriteResult result =
        InstallJump( site.m_ullSite, site.m_pFunc, &ullChain, nullptr, false, &reclaimRefusal,
                     REL32_JUMP_OPCODE );
    ReportReclaimWriteOutcome( site, result, reclaimRefusal );
    // A refused write leaves the site as the other module left it, so the caller's report stays true.
    if ( result == EWriteResult::Failed )
        return false;
    // Read once for both notes: another hand over could increment it while a format string reads it.
    const unsigned long ulHandedOver = site.m_ulCompletedHandOvers.load( std::memory_order_acquire );
    if ( ullChain == 0 )
    {
        // The site holds our branch with no target recorded. The recorded target stands: it is still where this entry goes.
        char szNote[384];
        _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                     "0x%08X now holds this plugin's branch, but the site no longer carried the branch that had displaced "
                     "this plugin by the time it was checked, so this attempt recorded no target; the target this site "
                     "records is left as it stands rather than cleared, and %lu frames had been handed over here by then",
                     static_cast<unsigned int>( site.m_ullSite ), ulHandedOver );
        ReportPatchNote( site.m_pName, szNote );
        site.m_ulStateReported.store( 0, std::memory_order_release );
        return true;
    }
    site.m_ullChainTarget.store( ullChain, std::memory_order_release );
    // Resolved from what the write recorded, not from the target handed in: the two differ if the site changed again.
    char szOwner[SITE_OWNER_NAME_LENGTH];
    DescribeAddressOwner( ullChain, szOwner, sizeof( szOwner ) );
    char szNote[384];
    _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                 "0x%08X had a branch from %s written over it by that module, so the site was taken back and the frame is "
                 "now handed to that branch after this plugin's own work, so both hooks keep running at this entry; the "
                 "handler had handed over %lu frames here by then",
                 static_cast<unsigned int>( site.m_ullSite ), szOwner, ulHandedOver );
    ReportPatchNote( site.m_pName, szNote );
    site.m_ulStateReported.store( 0, std::memory_order_release );  // the site is held again, so a later displacement is a new state
    return true;
}

// A site that stopped holding this plugin's branch is taken back within the budget; after that it is contested.
inline void RevalidateHeldCodeSites()
{
    const unsigned long ulCount = HeldCodeSiteCount().load( std::memory_order_acquire );
    SHeldCodeSite *pSites = HeldCodeSites();
    for ( unsigned long ulIndex = 0; ulIndex < ulCount; ++ulIndex )
    {
        SHeldCodeSite &site = pSites[ulIndex];
        UINT_PTR ullTarget = 0;
        const ECodeSiteState eState = ClassifyHeldCodeSite( site, ullTarget );
        if ( eState == ECodeSiteState::HoldsOurBranch )
            continue;
        if ( eState == ECodeSiteState::HoldsOtherBranch && AttemptReclaimCodeSite( site, ullTarget ) )
            continue;
        unsigned long ulOutcome = 0;
        switch ( eState )
        {
        case ECodeSiteState::Unreadable:
            ulOutcome = CODE_SITE_REPORTED_UNREADABLE;
            break;
        case ECodeSiteState::NotOurOpcode:
            ulOutcome = CODE_SITE_REPORTED_NOT_OURS;
            break;
        case ECodeSiteState::HoldsOtherBranch:
            ulOutcome = CODE_SITE_REPORTED_OTHER_BRANCH;
            break;
        case ECodeSiteState::HoldsOurBranch:
            continue;
        }
        if ( ( site.m_ulStateReported.load( std::memory_order_acquire ) & ulOutcome ) != 0 )
            continue;
        char szNote[320];
        if ( eState == ECodeSiteState::HoldsOtherBranch )
        {
            char szOwner[SITE_OWNER_NAME_LENGTH];
            DescribeAddressOwner( ullTarget, szOwner, sizeof( szOwner ) );
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "was a branch this plugin wrote into this executable and now carries one to 0x%08X in %s, so "
                         "the code this plugin replaced runs here again and it is not written back",
                         static_cast<unsigned int>( ullTarget ), szOwner );
        }
        else if ( eState == ECodeSiteState::NotOurOpcode )
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "was a branch this plugin wrote into this executable and no longer holds one, so another "
                         "module has rewritten the instruction at this site and the code this plugin replaced runs "
                         "here again instead of its hook" );
        else
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "was a branch this plugin wrote into this executable and stopped being readable memory during "
                         "a frame check, so whether its hook still runs at this site cannot be checked" );
        ReportPatchFailure( site.m_pName, reinterpret_cast<void *>( site.m_ullSite ), szNote );
        site.m_ulStateReported.fetch_or( ulOutcome, std::memory_order_acq_rel );  // after the report, so one that failed stays reportable
    }
}

// Called at the top of a frame and again at its end: verifying at install proves only the state at install.
inline void RevalidateExclusiveSlots()
{
    const unsigned long ulCount = ExclusiveSlotCount().load( std::memory_order_acquire );
    SExclusiveSlot *pSlots = ExclusiveSlots();
    for ( unsigned long ulIndex = 0; ulIndex < ulCount; ++ulIndex )
    {
        SExclusiveSlot &slot = pSlots[ulIndex];
        // Before any value is written back: four bytes written into an instruction another module replaced is broken code
        // in the middle of the executable's text, and leaving a site this plugin doesnt hold is the smaller of the two.
        const ESiteInstructionState instructionState = ClassifySiteInstruction( slot );
        if ( instructionState != ESiteInstructionState::Intact )
        {
            // One bit per state, as with the restore outcomes.
            unsigned long ulInstructionOutcome = 1UL << 3;
            const char *pInstructionReason =
                "was registered with an instruction prefix that describes no instruction, so the instruction it was "
                "written into can never be checked";
            if ( instructionState == ESiteInstructionState::Unreadable )
            {
                ulInstructionOutcome = 1UL << 0;
                pInstructionReason = "stopped being readable memory during a frame check, so the instruction it was "
                                     "written into cannot be read";
            }
            else if ( instructionState == ESiteInstructionState::Replaced )
            {
                ulInstructionOutcome = 1UL << 1;
                pInstructionReason = "no longer sits behind the instruction this plugin was checked against";
            }
            if ( ( slot.m_ulInstructionReported & ulInstructionOutcome ) == 0 )
            {
                // A fault rather than a note: while this stands the site is not held, so a callback the frame needs has
                // stopped reaching this renderer.
                char szNote[384];
                _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                             "%s, so it is not held at this plugin's value. Nothing here writes into it while that "
                             "stands, and the check looks every frame, so this line does not mean the site is lost "
                             "for the run", pInstructionReason );
                ReportPatchFailure( slot.m_pName, reinterpret_cast<void *>( slot.m_ullSite ), szNote );
                slot.m_ulInstructionReported |= ulInstructionOutcome;  // after the report, same reason
            }
            continue;
        }
        if ( slot.m_eKind == ESiteKind::AtLeastOurValue )
        {
            // Pinned read only for a row registration proved inside our own image, which is the only proof that lets the
            // frame loop skip a memory query. The test is repeated rather than trusted, so a row published by anything
            // other than RaiseIntLimit cant reach an unchecked read here.
            int iCurrent = 0;
            if ( slot.m_bSitePinned )
                iCurrent = ReadPinnedLimit( slot );
            else if ( !ReadSiteLimit( slot.m_ullSite, iCurrent ) )
            {
                if ( !slot.m_bUnreadableReported )
                {
                    // A fault for the reason the unreadable value site below gives: while this lasts nothing here writes
                    // the limit back, so a limit another module lowered stays lowered.
                    ReportPatchFailure( slot.m_pName, reinterpret_cast<void *>( slot.m_ullSite ),
                                        "stopped being readable memory during a frame check, so it is not held at this "
                                        "plugin's value while it stays unreadable. The check looks every frame, so this "
                                        "line does not mean the site is lost for the run" );
                    slot.m_bUnreadableReported = true;  // after the report, so one that failed stays reportable
                }
                continue;
            }
            if ( iCurrent >= slot.m_iFloor )
                continue;
            ++slot.m_ulDisplacements;
            SWriteRefusal limitRefusal;
            const EWriteResult limitResult =
                PatchHeldSite( slot.m_pName, slot.m_ullSite, &slot.m_iFloor, sizeof( slot.m_iFloor ), limitRefusal );
            // This branch owes the value branch's two reports: a raise that landed without finishing cleanly isnt the same
            // state as a refused one, so it gets its own clause rather than being called refused by the restore report below.
            if ( !slot.m_bLoweredReported || ( !slot.m_bLoweredRepeatReported && slot.m_ulDisplacements >= 2 ) )
            {
                const char *pRaise = "was written back";
                if ( limitResult == EWriteResult::WrittenNotClean )
                    pRaise = "was written back, though that write did not finish cleanly";
                else if ( limitResult == EWriteResult::Failed )
                    pRaise = "could not be written back";
                char szNote[384];
                // Which of the two lines is written is read once, so the flag selecting it is set after the line.
                const bool bFirstLowering = !slot.m_bLoweredReported;
                if ( bFirstLowering )
                {
                    _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                                 "was lowered to %d, which is below the %d this plugin asks for, and that raise %s. "
                                 "Reported once more if this happens again, and then raised without another line "
                                 "about it being lowered",
                                 iCurrent, slot.m_iFloor, pRaise );
                }
                else
                {
                    _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                                 "has been below the %d this plugin asks for on %lu checks since it was written, most "
                                 "recently reading %d, and that raise %s. No further line reports this limit being "
                                 "lowered, and the check keeps raising it every frame",
                                 slot.m_iFloor, slot.m_ulDisplacements, iCurrent, pRaise );
                }
                ReportPatchNote( slot.m_pName, szNote );
                // Marked after the report, so one that failed stays reportable.
                if ( bFirstLowering )
                    slot.m_bLoweredReported = true;
                else
                    slot.m_bLoweredRepeatReported = true;
            }
            if ( limitResult != EWriteResult::Clean )
                ReportRestoreOutcomeOnce( slot, limitResult, limitRefusal );
            continue;
        }
        UINT_PTR ullCurrent = 0;
        if ( slot.m_bSitePinned )
            ullCurrent = ReadPinnedSite( slot );
        else if ( !ReadSitePointer( slot.m_ullSite, ullCurrent ) )
        {
            // Named once: skipping this site silently would read exactly like a site still held at our value. A fault
            // rather than a note, because while it stays unreadable nothing here writes the value back.
            if ( !slot.m_bUnreadableReported )
            {
                ReportPatchFailure( slot.m_pName, reinterpret_cast<void *>( slot.m_ullSite ),
                                    "stopped being readable memory during a frame check, so it is not held at this "
                                    "plugin's value while it stays unreadable. The check looks every frame, so this "
                                    "line does not mean the site is lost for the run" );
                slot.m_bUnreadableReported = true;  // after the report, so one that failed stays reportable
            }
            continue;
        }
        if ( ullCurrent == slot.m_ullOurValue )
            continue;
        ++slot.m_ulDisplacements;
        // The write happens before the line describing it, because that line says what became of it: a note built first
        // would promise a write this attempt may have been refused.
        SWriteRefusal heldRefusal;
        const EWriteResult heldResult =
            PatchHeldSite( slot.m_pName, slot.m_ullSite, &slot.m_ullOurValue, sizeof( slot.m_ullOurValue ),
                           heldRefusal );
        // The module is named only when a report is about to be written. Describing it on every displaced frame would
        // query memory and format a path that is then thrown away.
        if ( !slot.m_bFirstReported || ( !slot.m_bRepeatReported && slot.m_ulDisplacements >= 2 ) )
        {
            char szOwner[SITE_OWNER_NAME_LENGTH];
            DescribeAddressOwner( ullCurrent, szOwner, sizeof( szOwner ) );
            // Three outcomes, not two: this write landed even when it didnt finish cleanly, and a line calling it refused
            // was contradicted by the restore report below. The reason is not promised here, because that report fires
            // once per distinct reason.
            const char *pOutcome = "and it was written back";
            if ( heldResult == EWriteResult::WrittenNotClean )
                pOutcome = "and it was written back, though that write did not finish cleanly";
            else if ( heldResult == EWriteResult::Failed )
                pOutcome = "and that write was refused";
            char szNote[384];
            // Which of the two lines is written is read once, so the flag selecting it is set after the line.
            const bool bFirstTaken = !slot.m_bFirstReported;
            if ( bFirstTaken )
            {
                _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                             "stopped holding this plugin's value after it was written and now reads 0x%08X from %s, "
                             "%s. Reported once more if this happens again, and then written back without another line "
                             "about it being taken",
                             static_cast<unsigned int>( ullCurrent ), szOwner, pOutcome );
            }
            else
            {
                _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                             "has not held this plugin's value on %lu checks since it was written, most recently reading "
                             "0x%08X from %s, %s. No further line reports this site being taken, and the check keeps "
                             "writing it back every frame",
                             slot.m_ulDisplacements, static_cast<unsigned int>( ullCurrent ), szOwner, pOutcome );
            }
            ReportPatchNote( slot.m_pName, szNote );
            // Marked after the report, so one that failed stays reportable.
            if ( bFirstTaken )
                slot.m_bFirstReported = true;
            else
                slot.m_bRepeatReported = true;
        }
        if ( heldResult != EWriteResult::Clean )
            ReportRestoreOutcomeOnce( slot, heldResult, heldRefusal );
    }
}

// Defined below, because it reads the two tables this sequence maintains, and reported from here rather than from a caller,
// so it runs on whichever frame path reaches us first.
inline void ReportSiteAuditOnce();

// Defined at the end of this file, and started from the sequence below. It exists because every channel we report a lost
// site through runs on a frame, and the state it has to report is frames having stopped reaching us.
inline void StartSiteWatchThread();

// Two threads can reach the frame path in a host that drives its own frames, and everything below mutates the two site
// tables and then reports through a logger that keeps one set of the messages it has written. Held for the whole sequence.
inline std::atomic<bool> g_bRevalidateBusy{ false };

struct SRevalidateSlot
{
    bool m_bHeld = false;

    SRevalidateSlot() = default;
    SRevalidateSlot( const SRevalidateSlot & ) = delete;
    SRevalidateSlot & operator=( const SRevalidateSlot & ) = delete;
    ~SRevalidateSlot()
    {
        Release();
    }

    bool Acquire()
    {
        bool bExpected = false;
        if ( !g_bRevalidateBusy.compare_exchange_strong( bExpected, true, std::memory_order_acq_rel ) )
            return false;
        m_bHeld = true;
        return true;
    }

    void Release()
    {
        if ( !m_bHeld )
            return;
        m_bHeld = false;
        g_bRevalidateBusy.store( false, std::memory_order_release );
    }
};

// The whole check in the order it has to run. A thread that cant take the slot skips the frame, as does a re-entrant call.
inline void RevalidateOwnedSites()
{
    SRevalidateSlot sequence;
    if ( !sequence.Acquire() )
        return;
    // Started here rather than from the loader, because a thread created under the loader lock is the shape we avoid
    // everywhere else. The first frame is early enough: the watch only has something to say once the tables are filling,
    // and that happens before any frame.
    StartSiteWatchThread();
    DeliverLatchedPatchNote();
    ReportSiteAuditOnce();
    RevalidateExclusiveSlots();
    RevalidateHeldCodeSites();
}

// Every site we own, with what it holds now and which module put it there, so a site taken before we reached it is a name
// in the log rather than an absent hook. Runs once, so it reads each site the careful way.
inline void ReportExclusiveSlotAudit()
{
    const unsigned long ulCount = ExclusiveSlotCount().load( std::memory_order_acquire );
    const SExclusiveSlot *pSlots = ExclusiveSlots();
    const unsigned long ulCodeCount = HeldCodeSiteCount().load( std::memory_order_acquire );
    const SHeldCodeSite *pCodeSites = HeldCodeSites();
    char szHeader[192];
    // Gated on a non empty table: a run where every value site was refused would otherwise announce that we check zero
    // sites we own, which reads as a plugin with nothing to check rather than as the run it was.
    if ( ulCount != 0 )
    {
        _snprintf_s( szHeader, sizeof( szHeader ), _TRUNCATE,
                     "checks the %lu fixed sites this plugin owns, which are the callbacks and pointer table entries it "
                     "has to hold and the limits it raises",
                     ulCount );
        ReportPatchNote( "Site audit", szHeader );
    }
    for ( unsigned long ulIndex = 0; ulIndex < ulCount; ++ulIndex )
    {
        const SExclusiveSlot &slot = pSlots[ulIndex];
        char szNote[384];
        if ( slot.m_eKind == ESiteKind::AtLeastOurValue )
        {
            // The careful read for every row, including ones registration proved: this audit runs once, so the queries
            // are paid once and its header promises exactly that.
            int iCurrent = 0;
            if ( !ReadSiteLimit( slot.m_ullSite, iCurrent ) )
            {
                _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                             "at 0x%08X is not readable memory, so the limit it holds could not be read",
                             static_cast<unsigned int>( slot.m_ullSite ) );
                ReportPatchNote( slot.m_pName, szNote );
                continue;
            }
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "at 0x%08X holds %d and this plugin asks for at least %d",
                         static_cast<unsigned int>( slot.m_ullSite ), iCurrent, slot.m_iFloor );
            ReportPatchNote( slot.m_pName, szNote );
            continue;
        }
        UINT_PTR ullCurrent = 0;
        // The same four way classification the install path uses: folding everything that isnt our value into "another
        // module's value" would report the executable's own value, which is what a site returns to, as a tool that took it.
        // An unreadable value gets its own line, since the zero this starts as would be recorded as the site's contents.
        if ( !ReadSitePointer( slot.m_ullSite, ullCurrent ) )
        {
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "at 0x%08X is not readable memory, so what it holds could not be read",
                         static_cast<unsigned int>( slot.m_ullSite ) );
            ReportPatchNote( slot.m_pName, szNote );
            continue;
        }
        char szVerdict[SITE_OWNER_NAME_LENGTH + 96];
        if ( ullCurrent == slot.m_ullOurValue )
            _snprintf_s( szVerdict, sizeof( szVerdict ), _TRUNCATE, "is this plugin's own value" );
        else if ( ullCurrent == 0 )
            _snprintf_s( szVerdict, sizeof( szVerdict ), _TRUNCATE, "is nothing, so this plugin writes over it" );
        else
        {
            switch ( ClassifyHostImage( reinterpret_cast<const void *>( ullCurrent ), sizeof( ullCurrent ) ) )
            {
            case EHostImage::Contained:
                _snprintf_s( szVerdict, sizeof( szVerdict ), _TRUNCATE,
                             "is this executable's own value, so this plugin writes over it" );
                break;
            case EHostImage::Outside:
            {
                char szOwner[SITE_OWNER_NAME_LENGTH];
                DescribeAddressOwner( ullCurrent, szOwner, sizeof( szOwner ) );
                _snprintf_s( szVerdict, sizeof( szVerdict ), _TRUNCATE,
                             "comes from %s and is another module's value, so this plugin writes over it", szOwner );
                break;
            }
            case EHostImage::Relocated:
            case EHostImage::Unknown:
                _snprintf_s( szVerdict, sizeof( szVerdict ), _TRUNCATE,
                             "is a value this plugin cannot attribute, because it could not read this executable's "
                             "own image, so it writes over it" );
                break;
            }
        }
        _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE, "at 0x%08X holds 0x%08X, which %s",
                     static_cast<unsigned int>( slot.m_ullSite ), static_cast<unsigned int>( ullCurrent ), szVerdict );
        ReportPatchNote( slot.m_pName, szNote );
    }
    // The code hooks are a second set answering a different question, so they get their own header rather than being
    // counted with the values above: these are branches written into the executable's own code, and a reader needs to know
    // whether each still holds the one it was given.
    if ( ulCodeCount != 0 )
    {
        char szCodeHeader[192];
        _snprintf_s( szCodeHeader, sizeof( szCodeHeader ), _TRUNCATE,
                     "checks the %lu code hooks this plugin wrote into this executable, which are branches it reads on "
                     "every frame and writes back only within its re-take budget",
                     ulCodeCount );
        ReportPatchNote( "Code audit", szCodeHeader );
    }
    for ( unsigned long ulIndex = 0; ulIndex < ulCodeCount; ++ulIndex )
    {
        const SHeldCodeSite &site = pCodeSites[ulIndex];
        UINT_PTR ullTarget = 0;
        char szNote[384];
        switch ( ClassifyHeldCodeSite( site, ullTarget ) )
        {
        case ECodeSiteState::HoldsOurBranch:
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE, "at 0x%08X holds this plugin's branch",
                         static_cast<unsigned int>( site.m_ullSite ) );
            break;
        case ECodeSiteState::HoldsOtherBranch:
            {
                char szOwner[SITE_OWNER_NAME_LENGTH];
                DescribeAddressOwner( ullTarget, szOwner, sizeof( szOwner ) );
                _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE, "at 0x%08X holds a branch to 0x%08X in %s",
                             static_cast<unsigned int>( site.m_ullSite ), static_cast<unsigned int>( ullTarget ),
                             szOwner );
            }
            break;
        case ECodeSiteState::NotOurOpcode:
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "at 0x%08X does not hold the branch this plugin wrote",
                         static_cast<unsigned int>( site.m_ullSite ) );
            break;
        case ECodeSiteState::Unreadable:
            _snprintf_s( szNote, sizeof( szNote ), _TRUNCATE,
                         "at 0x%08X is not memory this process can read, so the branch it holds was not checked",
                         static_cast<unsigned int>( site.m_ullSite ) );
            break;
        }
        ReportPatchNote( site.m_pName, szNote );
    }
}

// One report a run, and called from the guarded sequence above so the audit cant read a site while another thread is
// writing it back. The claim is given back rather than spent when the audit's lines did not reach the file.
inline void ReportSiteAuditOnce()
{
    // Three states rather than a bool: the claim has to be taken before the audit runs, so a bool would have to be spent
    // before knowing whether the lines landed, and would lose the audit for the run.
    enum : int
    {
        AUDIT_UNCLAIMED = 0,
        AUDIT_RUNNING = 1,
        AUDIT_DONE = 2
    };
    // Bounded, because a log that cant be written at all would otherwise be retried on every frame: a failed open per
    // frame is worse than the silence this escapes.
    constexpr unsigned long SITE_AUDIT_ATTEMPT_LIMIT = 8;
    static std::atomic<int> s_iAuditState{ AUDIT_UNCLAIMED };
    static std::atomic<unsigned long> s_ulAuditAttempts{ 0 };
    if ( s_iAuditState.load( std::memory_order_acquire ) == AUDIT_DONE )
        return;
    if ( ExclusiveSlotCount().load( std::memory_order_acquire ) == 0 &&
         HeldCodeSiteCount().load( std::memory_order_acquire ) == 0 )
        return;
    int iExpected = AUDIT_UNCLAIMED;
    if ( !s_iAuditState.compare_exchange_strong( iExpected, AUDIT_RUNNING, std::memory_order_acq_rel ) )
        return;
    const unsigned long ulAttempt = s_ulAuditAttempts.fetch_add( 1, std::memory_order_acq_rel ) + 1;
    BeginPatchNoteDeliveryCheck();
    ReportExclusiveSlotAudit();
    // Given back while attempts remain, so the next frame audits again rather than believing it reported. Reaching the
    // bound ends the audit with its lines undelivered: DONE means no further attempt, not that the audit reached the file.
    if ( !PatchNotesLanded() && ulAttempt < SITE_AUDIT_ATTEMPT_LIMIT )
    {
        s_iAuditState.store( AUDIT_UNCLAIMED, std::memory_order_release );
        return;
    }
    s_iAuditState.store( AUDIT_DONE, std::memory_order_release );
}

// Every channel we report a lost site through runs on a frame, and a host can take both away, after which nothing here
// runs and a displaced code hook is never named. This pass only reads the two tables, so it cant contend for a site: the
// reporting is what has to survive. A state is written only after two passes see it, because a site being put back right
// now reads as taken for a moment. Nothing here uses the logger, which the frame thread owns.
constexpr const char *SITE_WATCH_FILE_NAME = "gtaRenderHook_sitewatch.log";
constexpr DWORD SITE_WATCH_INTERVAL_MS = 500;
constexpr size_t SITE_WATCH_LINE_LENGTH = 320;
constexpr unsigned long SITE_WATCH_STATE_BITS = 5;
constexpr unsigned long MAX_SITE_WATCH_ROWS = MAX_EXCLUSIVE_SLOTS + MAX_HELD_CODE_SITES;

struct SSiteWatchRow
{
    const char *m_pName;
    UINT_PTR m_ullSite;
    // Zero for a value table row, one for a code hook row: the same state bit means a different thing in each, so a row
    // has to know which table it came from.
    unsigned long m_ulKind;
    unsigned long m_ulSeenThisPass;
    unsigned long m_ulSeenLastPass;
    unsigned long m_ulReported;
};

inline SSiteWatchRow *SiteWatchRows()
{
    static SSiteWatchRow s_aRows[MAX_SITE_WATCH_ROWS]{};
    return s_aRows;
}

inline std::atomic<unsigned long> &SiteWatchRowCount()
{
    static std::atomic<unsigned long> s_ulCount{ 0 };
    return s_ulCount;
}

// The bound below is the sum of both site tables, so this branch cant be reached while those two hold to their own bounds:
// it stays a refusal rather than an unchecked write, and says so in the file, because a bound that silently dropped a row
// would read exactly like a healthy site.
inline std::atomic<bool> g_bSiteWatchFullReported{ false };

inline bool AppendSiteWatchLine( const char *pLine )
{
    if ( pLine == nullptr || pLine[0] == '\0' )
        return false;
    // Opened, appended and closed under its own handle, so a file that cant be opened costs one call and leaves no state
    // behind. Appended rather than rewritten, because a crash mid-session must not lose the lines before it.
    FILE *pFile = nullptr;
    if ( fopen_s( &pFile, SITE_WATCH_FILE_NAME, "a" ) != 0 || pFile == nullptr )
        return false;
    // The append and the close are both checked, because the caller marks a state reported from this result. A state marked
    // after a write that didnt land is never written again, so the one line saying a site was lost is the line that goes
    // missing.
    const bool bAppended = fputs( pLine, pFile ) >= 0 && fputc( '\n', pFile ) != EOF;
    const bool bClosed = fclose( pFile ) == 0;
    return bAppended && bClosed;
}

inline bool WriteSiteWatchLine( const SSiteWatchRow &row, const char *pWhat )
{
    char line[SITE_WATCH_LINE_LENGTH];
    _snprintf_s( line, sizeof( line ), _TRUNCATE, "%s: the site at 0x%08X %s, seen on two checks of the site watch",
                 row.m_pName, static_cast<unsigned int>( row.m_ullSite ), pWhat );
    return AppendSiteWatchLine( line );
}

inline const char *SiteWatchStateText( unsigned long ulKind, unsigned long ulBit )
{
    if ( ulKind == 0 )
    {
        switch ( ulBit )
        {
        case 0:
            return "stopped being readable memory, so the instruction it was written into cannot be read and nothing "
                   "holds it at this plugin's value";
        case 1:
            return "no longer sits behind the instruction this plugin wrote into, so nothing holds it at this "
                   "plugin's value";
        case 2:
            return "was registered with an instruction prefix that describes no instruction, so the instruction it "
                   "was written into can never be checked";
        case 3:
            return "holds a limit below the one this plugin asks for";
        case 4:
            return "does not hold this plugin's value";
        default:
            return "is in a state this plugin does not own";
        }
    }
    switch ( ulBit )
    {
    case 0:
        return "stopped being readable memory, so the branch it holds could not be checked";
    case 1:
        return "no longer holds a branch this plugin wrote, so another module has rewritten the instruction there";
    case 2:
        return "carries another module's branch";
    default:
        return "is in a state this plugin does not own";
    }
}

// Accumulates what this pass saw. The state is held until the flush compares it against the previous pass, because one
// reading of a site the frame check is writing back isnt one that was lost.
inline void MarkSiteWatchState( const char *pName, UINT_PTR ullSite, unsigned long ulKind, unsigned long ulState )
{
    if ( ulState == 0 )
        return;
    SSiteWatchRow *pRows = SiteWatchRows();
    std::atomic<unsigned long> &ulCount = SiteWatchRowCount();
    const unsigned long ulRegistered = ulCount.load( std::memory_order_acquire );
    SSiteWatchRow *pRow = nullptr;
    for ( unsigned long ulIndex = 0; ulIndex < ulRegistered; ++ulIndex )
    {
        // Matched on the table the address came from as well as the address, because one state bit means a different thing
        // in each: a row found by address alone would describe a code hook row with the value table's words.
        if ( pRows[ulIndex].m_ullSite == ullSite && pRows[ulIndex].m_ulKind == ulKind )
        {
            pRow = &pRows[ulIndex];
            break;
        }
    }
    if ( pRow == nullptr )
    {
        if ( ulRegistered >= MAX_SITE_WATCH_ROWS )
            return;
        pRow = &pRows[ulRegistered];
        pRow->m_pName = pName;
        pRow->m_ullSite = ullSite;
        pRow->m_ulKind = ulKind;
        pRow->m_ulSeenThisPass = 0;
        pRow->m_ulSeenLastPass = 0;
        pRow->m_ulReported = 0;
        // Published last with the row, as both site tables do.
        ulCount.store( ulRegistered + 1, std::memory_order_release );
    }
    pRow->m_ulSeenThisPass |= ulState;
}

inline void FlushSiteWatchStates()
{
    SSiteWatchRow *pRows = SiteWatchRows();
    const unsigned long ulCount = SiteWatchRowCount().load( std::memory_order_acquire );
    // Cleared by the first refused write in a pass, so a path that cant be written costs one failed open per pass rather
    // than one per pending state. A state that was not written keeps its reported bit clear and is written on a later pass;
    // one that clears in between is never written, which is the cost of not retrying inside a pass.
    bool bWritable = true;
    for ( unsigned long ulIndex = 0; ulIndex < ulCount; ++ulIndex )
    {
        SSiteWatchRow &row = pRows[ulIndex];
        const unsigned long ulConfirmed = row.m_ulSeenThisPass & row.m_ulSeenLastPass & ~row.m_ulReported;
        for ( unsigned long ulBit = 0; ulBit < SITE_WATCH_STATE_BITS; ++ulBit )
        {
            const unsigned long ulOne = 1UL << ulBit;
            if ( ( ulConfirmed & ulOne ) == 0 || !bWritable )
                continue;
            // Recorded only for a line that reached the file, the ordering every report in the frame check uses: the mark
            // follows the report, so a state whose report didnt run stays reportable.
            if ( WriteSiteWatchLine( row, SiteWatchStateText( row.m_ulKind, ulBit ) ) )
                row.m_ulReported |= ulOne;
            else
                bWritable = false;
        }
        row.m_ulSeenLastPass = row.m_ulSeenThisPass;
        row.m_ulSeenThisPass = 0;
    }
    // Attempted here rather than where a site finds no room, so a full table costs one write per pass however many sites
    // arrive without a row. Marked once the line reached the file, so a run where the write fails reports it later.
    if ( !g_bSiteWatchFullReported.load( std::memory_order_acquire ) && ulCount >= MAX_SITE_WATCH_ROWS &&
         AppendSiteWatchLine( "Site watch: has no room left to record another site, so a site taken after this one "
                              "is not recorded in this file" ) )
        g_bSiteWatchFullReported.store( true, std::memory_order_release );
}

// The read only pass. It uses the same classifiers the frame check uses, so the two cant disagree about what a state means,
// and it takes no memory it doesnt read: ReadSiteLimit and ReadSitePointer ask the system about the address first, which
// is what makes this safe from a thread that doesnt hold the frame check's slot.
inline void SiteWatchPass()
{
    const unsigned long ulSlotCount = ExclusiveSlotCount().load( std::memory_order_acquire );
    SExclusiveSlot *pSlots = ExclusiveSlots();
    for ( unsigned long ulIndex = 0; ulIndex < ulSlotCount; ++ulIndex )
    {
        SExclusiveSlot &slot = pSlots[ulIndex];
        unsigned long ulState = 0;
        const ESiteInstructionState instructionState = ClassifySiteInstruction( slot );
        if ( instructionState == ESiteInstructionState::Unreadable )
            ulState |= 1UL << 0;
        else if ( instructionState == ESiteInstructionState::Replaced )
            ulState |= 1UL << 1;
        else if ( instructionState == ESiteInstructionState::Unverifiable )
            ulState |= 1UL << 2;
        else if ( slot.m_eKind == ESiteKind::AtLeastOurValue )
        {
            int iCurrent = 0;
            if ( ReadSiteLimit( slot.m_ullSite, iCurrent ) && iCurrent < slot.m_iFloor )
                ulState |= 1UL << 3;
        }
        else
        {
            UINT_PTR ullCurrent = 0;
            if ( ReadSitePointer( slot.m_ullSite, ullCurrent ) && ullCurrent != slot.m_ullOurValue )
                ulState |= 1UL << 4;
        }
        MarkSiteWatchState( slot.m_pName, slot.m_ullSite, 0, ulState );
    }
    const unsigned long ulCodeCount = HeldCodeSiteCount().load( std::memory_order_acquire );
    SHeldCodeSite *pSites = HeldCodeSites();
    for ( unsigned long ulIndex = 0; ulIndex < ulCodeCount; ++ulIndex )
    {
        SHeldCodeSite &site = pSites[ulIndex];
        UINT_PTR ullTarget = 0;
        unsigned long ulState = 0;
        switch ( ClassifyHeldCodeSite( site, ullTarget ) )
        {
        case ECodeSiteState::Unreadable:
            ulState = 1UL << 0;
            break;
        case ECodeSiteState::NotOurOpcode:
            ulState = 1UL << 1;
            break;
        case ECodeSiteState::HoldsOtherBranch:
            ulState = 1UL << 2;
            break;
        case ECodeSiteState::HoldsOurBranch:
            break;
        }
        MarkSiteWatchState( site.m_pName, site.m_ullSite, 1, ulState );
    }
    FlushSiteWatchStates();
}

inline std::atomic<bool> g_bSiteWatchStarted{ false };
inline std::atomic<bool> g_bSiteWatchStop{ false };
inline std::atomic<HANDLE> g_hSiteWatchStopEvent{ nullptr };
inline std::atomic<HANDLE> g_hSiteWatchThread{ nullptr };

inline DWORD WINAPI SiteWatchThreadProc( void * )
{
    for ( ;; )
    {
        // An event rather than a sleep, so the stop below wakes this at once instead of after the interval, and a frame-rate
        // host pays nothing: the thread is either waiting or reading, never spinning.
        const HANDLE hStop = g_hSiteWatchStopEvent.load( std::memory_order_acquire );
        if ( hStop == nullptr )
            return 0;
        switch ( WaitForSingleObject( hStop, SITE_WATCH_INTERVAL_MS ) )
        {
        case WAIT_OBJECT_0:
            return 0;
        case WAIT_TIMEOUT:
            break;
        default:
            // A wait that didnt wait returns at once, so carrying on would turn this into a loop reading every site as fast
            // as the processor allows, on a thread whose purpose is to cost the host nothing. The thread ends instead, and
            // the file is told, because a watch that stopped must be distinguishable from a run where nothing was taken.
            AppendSiteWatchLine( "Site watch: stopped waiting on its own event, so no site is watched from this thread "
                                 "for the rest of the run" );
            g_bSiteWatchStop.store( true, std::memory_order_release );
            return 0;
        }
        if ( g_bSiteWatchStop.load( std::memory_order_acquire ) )
            return 0;
        SiteWatchPass();
    }
}

// Called from the guarded frame sequence. Created lazily and once, because a thread created inside the loader is the shape
// we avoid everywhere else; a host that never reaches a frame path never gets one, and that is the one limit of this
// channel that is recorded rather than closed.
inline void StartSiteWatchThread()
{
    if ( g_bSiteWatchStop.load( std::memory_order_acquire ) )
        return;
    bool bExpectedStarted = false;
    if ( !g_bSiteWatchStarted.compare_exchange_strong( bExpectedStarted, true, std::memory_order_acq_rel ) )
        return;
    // Both failures below clear the started flag again, because a watch that never started is the one state this channel
    // cant report through its own file: the file is what didnt get written. Reported once rather than per frame, so a retry
    // doesnt cost a lock and a lookup on every frame where handles are scarce. Only one thread gets here, since its only
    // caller holds the frame check's slot.
    static std::atomic<bool> s_bSiteWatchStartFailureReported{ false };
    const HANDLE hStop = CreateEventA( nullptr, TRUE, FALSE, nullptr );
    if ( hStop == nullptr )
    {
        g_bSiteWatchStarted.store( false, std::memory_order_release );
        bool bExpectedFailure = false;
        if ( s_bSiteWatchStartFailureReported.compare_exchange_strong( bExpectedFailure, true,
                                                                      std::memory_order_acq_rel ) )
            ReportPatchNote( "Site watch",
                             "could not create the event its thread waits on, so no site is watched from another "
                             "thread and the site watch file stays empty; every later frame tries again" );
        return;
    }
    g_hSiteWatchStopEvent.store( hStop, std::memory_order_release );
    const HANDLE hThread = CreateThread( nullptr, 0, &SiteWatchThreadProc, nullptr, 0, nullptr );
    if ( hThread == nullptr )
    {
        // The event is closed again so the thread procedure's own guard cannot see a handle nothing will signal.
        g_hSiteWatchStopEvent.store( nullptr, std::memory_order_release );
        CloseHandle( hStop );
        g_bSiteWatchStarted.store( false, std::memory_order_release );
        bool bExpectedFailure = false;
        if ( s_bSiteWatchStartFailureReported.compare_exchange_strong( bExpectedFailure, true,
                                                                      std::memory_order_acq_rel ) )
            ReportPatchNote( "Site watch",
                             "could not create its thread, so no site is watched from another thread and the site "
                             "watch file stays empty; every later frame tries again" );
        return;
    }
    g_hSiteWatchThread.store( hThread, std::memory_order_release );
}

// Called from the detach path, before anything the thread could read is released. It reads only the two site tables, which
// are statics living for the whole process, so all this owes is that no more lines are written during teardown. Sets the
// stop and wakes the thread without waiting or touching a handle it might close, which is all a process already
// terminating may do.
inline void RequestStopSiteWatchThread()
{
    g_bSiteWatchStop.store( true, std::memory_order_release );
    const HANDLE hStop = g_hSiteWatchStopEvent.load( std::memory_order_acquire );
    if ( hStop != nullptr )
        SetEvent( hStop );
}

// The ordinary unload path: asks for the stop, waits for the thread to leave, and closes both handles.
inline void StopSiteWatchThread()
{
    RequestStopSiteWatchThread();
    const HANDLE hStop = g_hSiteWatchStopEvent.load( std::memory_order_acquire );
    const HANDLE hThread = g_hSiteWatchThread.load( std::memory_order_acquire );
    if ( hThread == nullptr )
        return;
    // A bounded wait, and the handles are left open when it expires: closing an event a running thread waits on is worse
    // than leaking two handles in a process already tearing down.
    if ( WaitForSingleObject( hThread, 5000 ) != WAIT_OBJECT_0 )
        return;
    CloseHandle( hThread );
    g_hSiteWatchThread.store( nullptr, std::memory_order_release );
    if ( hStop != nullptr )
    {
        CloseHandle( hStop );
        g_hSiteWatchStopEvent.store( nullptr, std::memory_order_release );
    }
}
