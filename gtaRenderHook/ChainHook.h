#pragma once

// Chained hook support.
//
// A code hook here is a five byte jump over a game function's entry, so a handler is entered with that function's
// frame. When another module already branched from the same entry this keeps that branch and hands the frame to it
// rather than refusing the site: the displaced branch replays the bytes it replaced and resumes inside the game
// function, so it is entered by a jump with [esp] rewritten rather than by a call.
//
// Adopting a site: arm the row InstallJump returned, then ChainBegin at the top of the handler and ChainHandOver
// before touching any local. The row starts at MAX_HELD_CODE_SITES so a refused install arms nothing.
//
// Handing over abandons the frame without unwinding it: no destructor runs, and every register the prologue pushed
// stays abandoned. Arming is therefore about generated code rather than source, and since no handler here proves it
// safe, nothing is armed and the re-take in MemoryFuncs.h has no row to act on. The test below is assembly for that
// reason.

#include "CDebug.h"
#include "MemoryFuncs.h"
#include <cstdio>
#include <cstring>
#include <intrin.h>

// Frames budgeted per site when sizing the shared stack. One site may hold more while others hold fewer; the total
// is what stops a runaway.
constexpr unsigned long CHAIN_FRAME_SIZING_PER_HELD_SITE = 8;
constexpr unsigned long CHAIN_FRAME_CAPACITY = MAX_HELD_CODE_SITES * CHAIN_FRAME_SIZING_PER_HELD_SITE;

// Zero until the install time test decides, one when a frame went all the way round with the right value and argument
// positions. Anything else makes ChainBegin refuse every frame and this plugin keeps its own hooks.
constexpr int CHAIN_VERDICT_UNTESTED = 0;
constexpr int CHAIN_VERDICT_USABLE   = 1;
constexpr int CHAIN_VERDICT_UNUSABLE = -1;
inline std::atomic<int> g_nChainHandOverVerdict{ CHAIN_VERDICT_UNTESTED };

// Whether the return slot may be rewritten. Only two outcomes, since a missing target and a full stack are both
// answered with zero by the arm rather than carried by a frame.
enum class EChainEntry
{
    Ready,             // the return slot holds an address this process can resume at, so a hand over may proceed
    NotAReturnAddress  // [esp] is not executable memory, or the hand over is unproven, so the frame was left alone
};

// One live frame. Only the saved return address is kept, since the continuation resumes whichever frame is deepest.
struct SChainFrameEntry
{
    UINT_PTR m_ullReturn;
};

// Frames for one thread, innermost last. A process wide one would resume another thread's return address.
struct SChainStack
{
    unsigned long m_ulDepth;
    SChainFrameEntry m_aFrames[ CHAIN_FRAME_CAPACITY ];
};

inline SChainStack &ChainStack()
{
    static thread_local SChainStack s_stack{};
    return s_stack;
}

static_assert( sizeof( UINT_PTR ) == 4, "the hand over restores esp from a return address slot, so it assumes 32 bit" );

// Rewrites the return slot and jumps to the displaced branch with esp back at its entry value, so this call's frame
// and the handler's are both discarded and the arguments stay where the displaced hook reads them. Naked for that
// reason, and one definition only: a naked function cant be declared naked, nor declared then defined inline.
__declspec( naked ) inline void ChainJumpTo( UINT_PTR, UINT_PTR, UINT_PTR )
{
    __asm
    {
        mov  eax, [ esp + 4 ]
        mov  ecx, [ esp + 8 ]
        mov  [ eax ], ecx
        mov  ecx, [ esp + 4 ]
        mov  eax, [ esp + 12 ]
        mov  esp, ecx
        jmp  eax
    }
}

// Installed in place of the return address, so the game function's own ret lands here. Saves the three registers a
// cdecl caller cant rely on across the post work call, so eax reaches the caller unchanged. Inline because its
// address goes into game frames and inline guarantees one address across the program.
extern "C" inline UINT_PTR __cdecl ChainRunPostWork( void );
__declspec( naked ) inline void ChainContinuation()
{
    __asm
    {
        push eax
        push ecx
        push edx
        call ChainRunPostWork
        pop  edx
        pop  ecx
        xchg eax, [ esp ]
        ret
    }
}

// A frame is usable only when [esp] holds an address this process can resume at. A function reached by a tail jump
// has the caller's data there. Must not be narrowed to the game's own image: a hosting client, another injected
// module and our own test all call in from their own module.
inline bool ChainReturnAddressUsable( const void *pReturnSlot )
{
    if ( pReturnSlot == nullptr )
        return false;
    if ( !IsReadableRange( pReturnSlot, sizeof( UINT_PTR ) ) )
        return false;
    UINT_PTR ullReturn = 0;
    memcpy( &ullReturn, pReturnSlot, sizeof( ullReturn ) );
    // One byte: the value is only resumed at, and a wider extent refuses a resumable address near a page end.
    return IsExecutableRange( reinterpret_cast<const void *>( ullReturn ), 1 );
}

// What a handler carries between ChainBegin and ChainHandOver. The site index is the row the site is held under,
// which is how the hand over finds the branch its frames go to. Nothing is pushed until the hand over, so a handler
// that finds nothing leaves no frame behind. Members are private and only the friends mint one, so a handler cant
// build a frame claiming a slot was checked. The defaults refuse, so a declared and never filled frame is declined.
class SChainFrame;

// Everything a hand over does except the jump, split out so the jump is made from a frame that saves nothing.
extern "C" __declspec( noinline ) inline UINT_PTR __cdecl ChainArmHandOver( const SChainFrame &frame, UINT_PTR ullChainTarget );

class SChainFrame
{
public:
    SChainFrame() = default;

private:
    friend SChainFrame ChainBegin( unsigned long ulSiteIndex, const char *pOperation );
    friend SChainFrame ChainBeginForSelfTest( void *pReturnSlot, unsigned long ulSiteIndex, const char *pOperation );
    friend bool ChainHandOver( const SChainFrame &frame, UINT_PTR ullChainTarget );
    friend UINT_PTR ChainArmHandOver( const SChainFrame &frame, UINT_PTR ullChainTarget );

    void *        m_pReturnSlot = nullptr;
    unsigned long m_ulSiteIndex = MAX_HELD_CODE_SITES;
    EChainEntry   m_eEntry = EChainEntry::NotAReturnAddress;
};

// Captures the frame and decides whether its return slot may be rewritten. Skips the verdict, so only two callers may
// reach it: ChainBegin after the gate, and ChainMakeSelfTestFrame, which is what the verdict is computed from. External
// linkage means nothing enforces that; the name is the whole warning.
__forceinline SChainFrame ChainBeginForSelfTest( void *pReturnSlot, unsigned long ulSiteIndex, const char *pOperation )
{
    SChainFrame frame;
    frame.m_pReturnSlot = pReturnSlot;
    frame.m_ulSiteIndex = ulSiteIndex;
    frame.m_eEntry = EChainEntry::NotAReturnAddress;

    if ( !ChainReturnAddressUsable( frame.m_pReturnSlot ) )
    {
        // One report per site per process. A store, not a count, so a site reached by a jump cant report again.
        static std::atomic<unsigned int> s_aReported[ MAX_HELD_CODE_SITES ]{};
        if ( ulSiteIndex < MAX_HELD_CODE_SITES &&
             s_aReported[ ulSiteIndex ].exchange( 1, std::memory_order_acq_rel ) == 0 && g_pDebug != nullptr )
        {
            char note[ 320 ];
            _snprintf_s( note, sizeof( note ), _TRUNCATE,
                         "%s: the top of this call frame does not hold an address this process can execute, which "
                         "means the function was reached by a jump rather than a call, so no displaced hook is "
                         "chained at this site and only this plugin's own work runs",
                         pOperation != nullptr ? pOperation : "Chain hook" );
            g_pDebug->printError( note );
        }
        return frame;
    }

    frame.m_eEntry = EChainEntry::Ready;
    return frame;
}

// Called at the top of a handler. Forced inline and it must stay that way: _AddressOfReturnAddress answers in the frame
// of the function its written in, so an outlined copy would report its own slot and hand over the wrong frame.
__forceinline SChainFrame ChainBegin( unsigned long ulSiteIndex, const char *pOperation )
{
    void *pReturnSlot = _AddressOfReturnAddress();

    // Anything but proven refuses, the untested zero included: unproven has to read the same way as failed.
    const int nVerdict = g_nChainHandOverVerdict.load( std::memory_order_acquire );
    if ( nVerdict != CHAIN_VERDICT_USABLE )
    {
        // Two reasons, two lines: untested means a handler ran before the test decided, an install order fault.
        if ( nVerdict == CHAIN_VERDICT_UNUSABLE )
        {
            static std::atomic<bool> s_bReportedUnusable{ false };
            if ( !s_bReportedUnusable.exchange( true, std::memory_order_acq_rel ) && g_pDebug != nullptr )
                g_pDebug->printError( "Chain hook: the install time test did not carry a frame round correctly, so no "
                                      "site hands its frames over and this plugin keeps its own hooks only" );
        }
        else
        {
            static std::atomic<bool> s_bReportedUntested{ false };
            if ( !s_bReportedUntested.exchange( true, std::memory_order_acq_rel ) && g_pDebug != nullptr )
                g_pDebug->printError( "Chain hook: a hook ran before the install time test had decided whether a frame "
                                      "can be handed over, so this frame was not handed over; that test is meant to run "
                                      "at install before the first hook is written" );
        }
        SChainFrame frame;
        frame.m_pReturnSlot = pReturnSlot;
        frame.m_ulSiteIndex = ulSiteIndex;
        frame.m_eEntry = EChainEntry::NotAReturnAddress;
        return frame;
    }

    return ChainBeginForSelfTest( pReturnSlot, ulSiteIndex, pOperation );
}

// Decides, pushes the frame and writes the continuation into the return slot. Answers the branch to jump to, or zero
// when the frame was not armed. A non zero answer is already the hand over, so every caller jumps. The row outranks
// the install time target, since a re-take leaves the row describing the branch that is there now. The push lives
// here, not in ChainBegin: a frame pushed there and then declined here would never be popped.
extern "C" __declspec( noinline ) inline UINT_PTR __cdecl ChainArmHandOver( const SChainFrame &frame, UINT_PTR ullChainTarget )
{
    if ( frame.m_eEntry != EChainEntry::Ready )
        return 0;
    const UINT_PTR ullRecorded = HeldCodeSiteChainTarget( frame.m_ulSiteIndex );
    if ( ullRecorded != 0 )
        ullChainTarget = ullRecorded;
    if ( ullChainTarget == 0 )
        return 0;

    SChainStack &stack = ChainStack();
    if ( stack.m_ulDepth >= CHAIN_FRAME_CAPACITY )
    {
        // The caller keeps its own hook and chains again once held frames drain. Reported once, since a full stack is
        // the host's pumping depth rather than one site's.
        static std::atomic<bool> s_bReportedFull{ false };
        if ( !s_bReportedFull.exchange( true, std::memory_order_acq_rel ) && g_pDebug != nullptr )
            g_pDebug->printError( "Chain hook: the table of live chained frames is full, so this frame was not handed "
                                  "over; the frames already held keep their hand over and this site chains again once "
                                  "enough of them have returned" );
        return 0;
    }
    UINT_PTR ullReturn = 0;
    memcpy( &ullReturn, frame.m_pReturnSlot, sizeof( ullReturn ) );
    stack.m_aFrames[ stack.m_ulDepth ].m_ullReturn = ullReturn;
    ++stack.m_ulDepth;
    // Recorded before the jump, since the jump doesnt come back. This count is the evidence a re-take is allowed on.
    NoteHeldCodeSiteHandOver( frame.m_ulSiteIndex );
    // Slot written last: arming then not jumping would leave the entry resuming with no frame behind it.
    const UINT_PTR ullContinuation = reinterpret_cast<UINT_PTR>( &ChainContinuation );
    memcpy( frame.m_pReturnSlot, &ullContinuation, sizeof( ullContinuation ) );
    return ullChainTarget;
}

// Hands the frame to the branch this site is held against. False means nothing changed and locals stay usable; true
// means the frame is gone, no destructor will run, and those locals are off limits. Only from a frame that saves nothing.
inline bool ChainHandOver( const SChainFrame &frame, UINT_PTR ullChainTarget )
{
    const UINT_PTR ullTarget = ChainArmHandOver( frame, ullChainTarget );
    if ( ullTarget == 0 )
        return false;
    ChainJumpTo( reinterpret_cast<UINT_PTR>( frame.m_pReturnSlot ),
                 reinterpret_cast<UINT_PTR>( &ChainContinuation ), ullTarget );
    return true;
}

// Pops this thread's innermost frame and answers the address to resume at. Called only by the continuation.
extern "C" inline UINT_PTR __cdecl ChainRunPostWork( void )
{
    SChainStack &stack = ChainStack();
    if ( stack.m_ulDepth == 0 )
    {
        // Nothing can name the address to resume: the frame that held it is gone and this return value becomes the
        // instruction pointer. Trap, and repeat, since a break alone resumes at the same ret.
        if ( g_pDebug != nullptr )
            g_pDebug->printError( "Chain hook: a continuation ran with no live frame on this thread, so the address it "
                                  "would resume is unknown; the process is stopped here rather than resumed at a guess" );
        for ( ;; )
            __debugbreak();
        __assume( 0 );
    }
    --stack.m_ulDepth;
    return stack.m_aFrames[ stack.m_ulDepth ].m_ullReturn;
}

// A whole hand over, exercised at run time rather than only compiled. The body below is entered by an ordinary call,
// so its frame is the one a displaced branch sees, and the stand in returns a value the body never returns, so the
// check can fail. The index sits outside the held site table so the per site report array spends nothing.
constexpr unsigned long CHAIN_SELF_TEST_SITE = MAX_HELD_CODE_SITES;
constexpr int CHAIN_SELF_TEST_ARGUMENT_A = 7;
constexpr int CHAIN_SELF_TEST_ARGUMENT_B = 11;
constexpr int CHAIN_SELF_TEST_DISPLACED_RESULT = CHAIN_SELF_TEST_ARGUMENT_A * 100 + CHAIN_SELF_TEST_ARGUMENT_B;
constexpr unsigned long CHAIN_SELF_TEST_DISPLACED_RUNS = 1;

inline std::atomic<unsigned long> g_ulChainSelfTestDisplacedRuns{ 0 };

inline int __cdecl ChainSelfTestDisplaced( int nA, int nB )
{
    g_ulChainSelfTestDisplacedRuns.fetch_add( 1, std::memory_order_acq_rel );
    return nA * 100 + nB;
}

// The three things the naked body calls, published as addresses: taking one makes the compiler emit a body for an
// inline function nothing else calls, and a plain C name lets the assembly reach it.
extern "C" inline UINT_PTR g_ullChainMakeFrameFunction = 0;
extern "C" inline UINT_PTR g_ullChainArmFunction = 0;
extern "C" inline UINT_PTR g_ullChainSelfTestTarget = 0;

extern "C" __declspec( noinline ) inline void __cdecl ChainMakeSelfTestFrame( SChainFrame *pOut, void *pReturnSlot )
{
    *pOut = ChainBeginForSelfTest( pReturnSlot, CHAIN_SELF_TEST_SITE, "Chain self test" );
}

// Not inlined and naked, and both must stay that way: the slot it rewrites has to be its own, and the hand over abandons
// this frame so a prologue's pushes would never be popped. [esp] is that slot, [esp+4]/[esp+8] the arguments the
// displaced branch reads. Both call pointers are tested: a call through a zero slot faults in the loader with nothing
// logged, while declining returns the sum and fails the comparison. The scratch is an enum, not a constexpr, since the
// assembler resolves this name as a number and a constexpr as a memory reference.
enum : unsigned long { CHAIN_SELF_TEST_SCRATCH_BYTES = 16 };
static_assert( CHAIN_SELF_TEST_SCRATCH_BYTES % 4 == 0,
               "the body leaves esp on a multiple of four so both calls see the alignment they would have had without "
               "the scratch, and the displaced branch reads its arguments from that same stack" );
static_assert( sizeof( SChainFrame ) <= CHAIN_SELF_TEST_SCRATCH_BYTES,
               "the body writes a whole SChainFrame into that much scratch, so a larger one would overwrite its own "
               "arguments and the return address instead of failing here" );
#pragma warning( push )
#pragma warning( disable : 4100 )  // nA and nB are read by the displaced branch from the stack, not from here
__declspec( naked ) inline int __cdecl ChainSelfTestBody( int nA, int nB )
{
    __asm
    {
        sub  esp, CHAIN_SELF_TEST_SCRATCH_BYTES
        cmp  dword ptr [ g_ullChainMakeFrameFunction ], 0
        je   ChainSelfTestBodyDeclined
        cmp  dword ptr [ g_ullChainArmFunction ], 0
        je   ChainSelfTestBodyDeclined
        lea  eax, [ esp + CHAIN_SELF_TEST_SCRATCH_BYTES ]
        mov  edx, esp
        push eax
        push edx
        call dword ptr [ g_ullChainMakeFrameFunction ]
        add  esp, 8
        push dword ptr [ g_ullChainSelfTestTarget ]
        lea  eax, [ esp + 4 ]
        push eax
        call dword ptr [ g_ullChainArmFunction ]
        add  esp, 8
        test eax, eax
        je   ChainSelfTestBodyDeclined
        add  esp, CHAIN_SELF_TEST_SCRATCH_BYTES
        jmp  eax
    ChainSelfTestBodyDeclined:
        add  esp, CHAIN_SELF_TEST_SCRATCH_BYTES
        mov  eax, dword ptr [ esp + 4 ]
        add  eax, dword ptr [ esp + 8 ]
        ret
    }
}
#pragma warning( pop )

// Runs the install time test and publishes its verdict. Must run before the first hook is installed, and at most once
// for the process with the guard here rather than at the call sites: the comparison counts runs against one, so a second
// run would see two and fail on its own counter. A caller arriving mid test gets false, meaning not proven yet.
inline bool RunChainSelfTest( const char *pOperation )
{
    static std::atomic<bool> s_bRan{ false };
    if ( !s_bRan.exchange( true, std::memory_order_acq_rel ) )
    {
        // Published before the body runs: it reaches all three through these rather than through names.
        g_ullChainMakeFrameFunction = reinterpret_cast<UINT_PTR>( &ChainMakeSelfTestFrame );
        g_ullChainArmFunction = reinterpret_cast<UINT_PTR>( &ChainArmHandOver );
        g_ullChainSelfTestTarget = reinterpret_cast<UINT_PTR>( &ChainSelfTestDisplaced );
        const int nResult = ChainSelfTestBody( CHAIN_SELF_TEST_ARGUMENT_A, CHAIN_SELF_TEST_ARGUMENT_B );
        const unsigned long ulRuns = g_ulChainSelfTestDisplacedRuns.load( std::memory_order_acquire );
        const bool bOk = nResult == CHAIN_SELF_TEST_DISPLACED_RESULT && ulRuns == CHAIN_SELF_TEST_DISPLACED_RUNS;
        g_nChainHandOverVerdict.store( bOk ? CHAIN_VERDICT_USABLE : CHAIN_VERDICT_UNUSABLE, std::memory_order_release );
        if ( !bOk && g_pDebug != nullptr )
        {
            char note[ 288 ];
            _snprintf_s( note, sizeof( note ), _TRUNCATE,
                         "%s: a chained frame returned %d and ran the displaced branch %lu times, where a correct hand over "
                         "returns %d and runs it once, so the frame hand over is not usable in this build and no site chains",
                         pOperation != nullptr ? pOperation : "Chain hook", nResult, ulRuns,
                         CHAIN_SELF_TEST_DISPLACED_RESULT );
            g_pDebug->printError( note );
        }
    }
    return g_nChainHandOverVerdict.load( std::memory_order_acquire ) == CHAIN_VERDICT_USABLE;
}
