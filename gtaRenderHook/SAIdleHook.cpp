// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "SAIdleHook.h"
#include "DeferredRenderer.h"
#include "ShadowRenderer.h"
#include "Renderer.h"
#include "D3D1XStateManager.h"
#include "D3D1XRenderBuffersManager.h"
#include "VoxelOctreeRenderer.h"
#include "LightManager.h"
#include "HDRTonemapping.h"
#include "CustomCarFXPipeline.h"
#include "CustomBuildingPipeline.h"
#include "CustomBuildingDNPipeline.h"
#include "CWaterLevel.h"
#include "RwMethods.h"
#include <AntTweakBar.h>
#include "DebugRendering.h"
#include "D3DRenderer.h"
#include "DebugBBox.h"
#include "CVisibilityPluginsRH.h"
#include "AmbientOcclusion.h"
#include "CloudRendering.h"
#include <game_sa\CPointLights.h> 
#include <game_sa\CFont.h>
#include <game_sa\CWorld.h>
#include <game_sa\CScene.h>
#include <game_sa\CClock.h>
#include <game_sa\CTimer.h>
#include <game_sa\CStreaming.h>
#include <game_sa\CClouds.h>
#include <game_sa\CWeaponEffects.h>
#include <game_sa\CSpecialFX.h>
#include <game_sa\CCoronas.h>
#include <game_sa\Fx_c.h>
#include <game_sa\CGame.h>
#include <game_sa\CDraw.h>
#include <game_sa\CVisibilityPlugins.h>
#include <game_sa\CBirds.h>
#include <game_sa\CRopes.h>
#include <game_sa\CRenderer.h>
#include "VolumetricLighting.h"
#include "FullscreenQuad.h"
#include "CubemapReflectionRenderer.h"
#include "TemporalAA.h"
#include <type_traits>
#include <atomic>
//#include <game_sa\CGlass.h>
//#include <game_sa\CSkidmarks.h>


/* GTA Definitions TODO: Move somewhere else*/
#define g_breakMan ((void *)0xBB4240)		// Break manager
constexpr UINT_PTR IDLE_FUNCTION_ADDRESS = 0x53E920;

// Hook the entry, not a call site: a host that owns the message pump may call the function directly.
constexpr SIZE_T IDLE_PROLOGUE_LENGTH = 16;

// The first sixteen bytes of the idle entry, one instruction per line, read from gta_sa.exe.lst.
static constexpr unsigned char g_aExpectedIdlePrologue[] =
{
    0x83, 0xEC, 0x08,               // sub esp, 8
    0xE8, 0x58, 0x31, 0x02, 0x00,   // call CTimer::GetCurrentTimeInCycles (0x561A80)
    0xE8, 0x13, 0x31, 0x02, 0x00,   // call CTimer::GetCyclesPerMillisecond (0x561A40)
    0x56,                           // push esi
    0x8B, 0xFF                      // two byte no-op that pads the branch target
};

// Sized by its own list, so a trimmed list fails here rather than padding with zeros.
static_assert( sizeof( g_aExpectedIdlePrologue ) == IDLE_PROLOGUE_LENGTH,
               "the tail report compares to the end of this list, so the list has to hold exactly the bytes the "
               "length names" );

// sub esp, imm8 is one opcode byte, one modrm byte and one immediate byte.
constexpr UINT_PTR IDLE_SUB_ESP_OPCODE_LENGTH    = 1;
constexpr UINT_PTR IDLE_SUB_ESP_MODRM_LENGTH     = 1;
constexpr UINT_PTR IDLE_SUB_ESP_IMMEDIATE_LENGTH = 1;

constexpr UINT_PTR IDLE_SUB_ESP_LENGTH =
    IDLE_SUB_ESP_OPCODE_LENGTH + IDLE_SUB_ESP_MODRM_LENGTH + IDLE_SUB_ESP_IMMEDIATE_LENGTH;
constexpr UINT_PTR IDLE_SUB_ESP_IMMEDIATE_OFFSET = IDLE_SUB_ESP_LENGTH - 1;

// The assembler reads a bare identifier as a memory operand, so the immediate has to arrive as macro text.
#define IDLE_SUB_ESP_ALLOCATION_IMM 8
constexpr int IDLE_SUB_ESP_ALLOCATION = IDLE_SUB_ESP_ALLOCATION_IMM;

static_assert( IDLE_SUB_ESP_ALLOCATION > 0 && IDLE_SUB_ESP_ALLOCATION <= 127,
               "the replayed sub esp takes a signed eight bit immediate, so the allocation has to fit in one" );

constexpr UINT_PTR IDLE_REPLAYED_LENGTH = IDLE_SUB_ESP_LENGTH + REL32_BRANCH_LENGTH;

// The window runs to the end of the instruction the body resumes into: its operand chooses the callee.
constexpr SIZE_T IDLE_VERIFIED_LENGTH = static_cast<SIZE_T>( IDLE_REPLAYED_LENGTH ) + REL32_BRANCH_LENGTH;

constexpr UINT_PTR IDLE_RESUME_ADDRESS = IDLE_FUNCTION_ADDRESS + IDLE_REPLAYED_LENGTH;
#define IDLE_RESUME_ADDRESS_IMM 0x53E928
constexpr UINT_PTR IDLE_RESUME_ADDRESS_ASM = IDLE_RESUME_ADDRESS_IMM;

static_assert( IDLE_RESUME_ADDRESS == IDLE_RESUME_ADDRESS_ASM,
               "the instruction after the replayed call sits here in the listing, so the lengths above must "
               "describe the instructions the trampoline actually replays" );
static_assert( static_cast<UINT_PTR>( REL32_BRANCH_LENGTH ) <=
                   IDLE_RESUME_ADDRESS_ASM - IDLE_FUNCTION_ADDRESS,
               "the jump ends at or before the resume point, so it writes only the bytes the trampoline replays "
               "itself and stops short of the instruction the game resumes at" );
static_assert( g_aExpectedIdlePrologue[0] == 0x83 && g_aExpectedIdlePrologue[1] == 0xEC,
               "the trampoline replays sub esp first, so these two bytes are not free to change" );
static_assert( g_aExpectedIdlePrologue[IDLE_SUB_ESP_IMMEDIATE_OFFSET] == IDLE_SUB_ESP_ALLOCATION,
               "the trampoline's sub esp must allocate what this byte encodes, or the resumed body reads its "
               "argument from the wrong slot" );
static_assert( g_aExpectedIdlePrologue[IDLE_SUB_ESP_LENGTH] == REL32_CALL_OPCODE,
               "the replayed call starts where sub esp ends and its target is the operand after this byte, so "
               "neither the length above nor this opcode can be wrong on its own" );
static_assert( g_aExpectedIdlePrologue[IDLE_REPLAYED_LENGTH] == REL32_CALL_OPCODE,
               "the trampoline resumes exactly where the replayed call ends, so the second call has to start "
               "there or the resumed body is entered mid instruction" );

// Unsigned add, so a backward call reaches its target by wrapping.
constexpr UINT_PTR IDLE_INNER_CALL_DISPLACEMENT_OFFSET = IDLE_SUB_ESP_LENGTH + REL32_OPCODE_LENGTH;

constexpr uint32_t IDLE_INNER_CALL_DISPLACEMENT =
    static_cast<uint32_t>( g_aExpectedIdlePrologue[IDLE_INNER_CALL_DISPLACEMENT_OFFSET] )
    | ( static_cast<uint32_t>( g_aExpectedIdlePrologue[IDLE_INNER_CALL_DISPLACEMENT_OFFSET + 1] ) << 8 )
    | ( static_cast<uint32_t>( g_aExpectedIdlePrologue[IDLE_INNER_CALL_DISPLACEMENT_OFFSET + 2] ) << 16 )
    | ( static_cast<uint32_t>( g_aExpectedIdlePrologue[IDLE_INNER_CALL_DISPLACEMENT_OFFSET + 3] ) << 24 );

// Decoded from the pattern, so the target and the verified bytes cannot drift apart.
constexpr UINT_PTR IDLE_INNER_CALL_IMM = 0x561A80;
constexpr UINT_PTR IDLE_INNER_CALL = IDLE_RESUME_ADDRESS + IDLE_INNER_CALL_DISPLACEMENT;

static_assert( IDLE_INNER_CALL == IDLE_INNER_CALL_IMM,
               "the replayed call's operand has to decode to CTimer::GetCurrentTimeInCycles at .text:00561A80, so "
               "those four bytes are not free to change and the trampoline cannot be aimed at another target" );

constexpr UINT_PTR IDLE_CYCLES_PER_MS = 0x561A40;
constexpr UINT_PTR IDLE_CYCLES_PER_MS_DISPLACEMENT_OFFSET = IDLE_REPLAYED_LENGTH + REL32_OPCODE_LENGTH;

constexpr uint32_t IDLE_CYCLES_PER_MS_DISPLACEMENT =
    static_cast<uint32_t>( g_aExpectedIdlePrologue[IDLE_CYCLES_PER_MS_DISPLACEMENT_OFFSET] )
    | ( static_cast<uint32_t>( g_aExpectedIdlePrologue[IDLE_CYCLES_PER_MS_DISPLACEMENT_OFFSET + 1] ) << 8 )
    | ( static_cast<uint32_t>( g_aExpectedIdlePrologue[IDLE_CYCLES_PER_MS_DISPLACEMENT_OFFSET + 2] ) << 16 )
    | ( static_cast<uint32_t>( g_aExpectedIdlePrologue[IDLE_CYCLES_PER_MS_DISPLACEMENT_OFFSET + 3] ) << 24 );

static_assert( IDLE_CYCLES_PER_MS ==
                   IDLE_RESUME_ADDRESS + REL32_BRANCH_LENGTH + IDLE_CYCLES_PER_MS_DISPLACEMENT,
               "the second call's operand has to decode to the function the listing names there, so those four "
               "bytes are part of what identifies this body and are not free to change" );

static_assert( IDLE_VERIFIED_LENGTH <= IDLE_PROLOGUE_LENGTH,
               "the gate cannot ask more of the entry than the pattern it compares against describes" );
static_assert( IDLE_VERIFIED_LENGTH < IDLE_PROLOGUE_LENGTH,
               "the tail report compares from the end of the verified window to the end of the pattern, so the "
               "pattern has to carry bytes past that window or the report has nothing to read and stops reporting "
               "without any other check noticing" );
static_assert( IDLE_VERIFIED_LENGTH == IDLE_REPLAYED_LENGTH + REL32_BRANCH_LENGTH,
               "the window has to end with the instruction the body resumes into, so the operand that chooses that "
               "call's callee is verified and not only its opcode" );

// These three are pinned here because they carry no operand.
static_assert( IDLE_PROLOGUE_LENGTH - IDLE_VERIFIED_LENGTH == 3,
               "the three asserts below pin the whole tail the install tolerates, so a tail of any other length "
               "would leave tolerated bytes that nothing describes" );
static_assert( g_aExpectedIdlePrologue[IDLE_VERIFIED_LENGTH] == 0x56,
               "the first tolerated byte is push esi, which carries no operand, so the tolerated tail begins with "
               "an instruction that cannot aim the body anywhere" );
static_assert( g_aExpectedIdlePrologue[IDLE_VERIFIED_LENGTH + 1] == 0x8B &&
                   g_aExpectedIdlePrologue[IDLE_VERIFIED_LENGTH + 2] == 0xFF,
               "the last two tolerated bytes are the two byte no-op that pads the branch target, and a pattern "
               "that put an instruction with an operand here would be tolerating bytes able to redirect the body" );

static_assert( IDLE_INNER_CALL_DISPLACEMENT_OFFSET + static_cast<UINT_PTR>( sizeof( uint32_t ) ) <=
                   IDLE_PROLOGUE_LENGTH,
               "this decode reads four bytes from the pattern, so all four have to lie inside it" );
static_assert( IDLE_CYCLES_PER_MS_DISPLACEMENT_OFFSET + static_cast<UINT_PTR>( sizeof( uint32_t ) ) <=
                   IDLE_PROLOGUE_LENGTH,
               "this decode reads four bytes from the pattern, so all four have to lie inside it" );

// Patch() compares only the first IDLE_VERIFIED_LENGTH bytes, so this is the assert that fails if the window narrows.
static_assert( IDLE_CYCLES_PER_MS_DISPLACEMENT_OFFSET + static_cast<UINT_PTR>( sizeof( uint32_t ) ) <=
                   IDLE_VERIFIED_LENGTH,
               "the second call's operand is inside the window the gate verifies, so the decode has to end at or "
               "before the end of that window and not merely inside the pattern" );

static constexpr unsigned char g_aExpectedInnerCallPrologue[] =
{
    0x83, 0xEC, 0x08,                     // sub esp, 8
    0x8D, 0x44, 0x24, 0x00,               // lea eax, [esp]
    0x50,                                 // push eax
    0xFF, 0x15, 0xC8, 0x80, 0x85, 0x00,   // call ds:__imp_QueryPerformanceCounter
    0x8B, 0x44                            // mov eax, [esp+8+PerformanceCount], cut at its first two bytes
};

static_assert( sizeof( g_aExpectedInnerCallPrologue ) == 16,
               "the install time check compares sixteen bytes of the trampoline target" );

// Checked at build time against the bytes it was read from; the offset is a sum of the encoded lengths.
constexpr UINT_PTR IDLE_QUERY_PERFORMANCE_COUNTER_SLOT = 0x008580C8;

constexpr UINT_PTR IDLE_INNER_CALL_INDIRECT_OPCODE_LENGTH = 1;
constexpr UINT_PTR IDLE_INNER_CALL_INDIRECT_MODRM_LENGTH  = 1;
constexpr UINT_PTR IDLE_INNER_CALL_LEA_ESP_LENGTH         = 4;
constexpr UINT_PTR IDLE_INNER_CALL_PUSH_ESP_LENGTH        = 1;

constexpr UINT_PTR IDLE_INNER_CALL_OPCODE_OFFSET =
    IDLE_SUB_ESP_LENGTH + IDLE_INNER_CALL_LEA_ESP_LENGTH + IDLE_INNER_CALL_PUSH_ESP_LENGTH;
constexpr UINT_PTR IDLE_INNER_CALL_OPERAND_OFFSET =
    IDLE_INNER_CALL_OPCODE_OFFSET + IDLE_INNER_CALL_INDIRECT_OPCODE_LENGTH +
    IDLE_INNER_CALL_INDIRECT_MODRM_LENGTH;

static_assert( g_aExpectedInnerCallPrologue[IDLE_INNER_CALL_OPCODE_OFFSET] == 0xFF &&
               g_aExpectedInnerCallPrologue[IDLE_INNER_CALL_OPCODE_OFFSET + IDLE_INNER_CALL_INDIRECT_OPCODE_LENGTH]
                   == 0x15,
               "the operand below is read as an indirect call's address, so these two bytes have to encode that call" );
static_assert( IDLE_INNER_CALL_OPERAND_OFFSET + static_cast<UINT_PTR>( sizeof( uint32_t ) ) <=
                   sizeof( g_aExpectedInnerCallPrologue ),
               "this decode reads four bytes from the pattern, so all four have to lie inside it" );

constexpr uint32_t IDLE_QUERY_PERFORMANCE_COUNTER_SLOT_ENCODED =
    static_cast<uint32_t>( g_aExpectedInnerCallPrologue[IDLE_INNER_CALL_OPERAND_OFFSET] )
    | ( static_cast<uint32_t>( g_aExpectedInnerCallPrologue[IDLE_INNER_CALL_OPERAND_OFFSET + 1] ) << 8 )
    | ( static_cast<uint32_t>( g_aExpectedInnerCallPrologue[IDLE_INNER_CALL_OPERAND_OFFSET + 2] ) << 16 )
    | ( static_cast<uint32_t>( g_aExpectedInnerCallPrologue[IDLE_INNER_CALL_OPERAND_OFFSET + 3] ) << 24 );

static_assert( IDLE_QUERY_PERFORMANCE_COUNTER_SLOT == IDLE_QUERY_PERFORMANCE_COUNTER_SLOT_ENCODED,
               "the named slot and the operand in the pattern are one address, so a mistyped digit fails this build" );
static_assert( sizeof( void * ) == 4 && sizeof( UINT_PTR ) == 4,
               "the trampoline pushes a 32 bit address and resumes at one, which assume these widths" );
static_assert( std::is_same_v<decltype( &CSAIdleHook::Idle ), void ( __cdecl * )( void * )>,
               "the games callers reach the redirected entry point directly, so it has to stay __cdecl" );

// Memory indirect, since a rel32 cannot reach a module the loader may move. The push reproduces what a call would
// have left behind; holds only while gta_sa.exe loads at 0x00400000.
static void *const g_pIdleInnerCall = reinterpret_cast<void *>( IDLE_INNER_CALL );
__declspec( naked ) static void __cdecl IdleOriginalNaked( void * )
{
    __asm
    {
        sub  esp, IDLE_SUB_ESP_ALLOCATION_IMM
        push IDLE_RESUME_ADDRESS_IMM
        jmp  dword ptr [g_pIdleInnerCall]
    }
}
#undef IDLE_SUB_ESP_ALLOCATION_IMM
#undef IDLE_RESUME_ADDRESS_IMM

#define _Idle( ptr ) IdleOriginalNaked( ptr )
// Both pause flags come from the SDK's own unit, so this file keeps no copy of either address.
#define CPostEffects__m_bDisableAllPostEffect (*(bool *)0xC402CF)	

int drawCallCount = 0;
float CSAIdleHook::m_fShadowDNBalance = 1.0;

// CGrassRenderer sets each plant type's colour with RpGeometryForAllMaterials( geometry, 0x5DAC10, &colour ). Its alpha
// comes from plants.dat (about 40/255, faded with distance) and is meant for soft blending, but the building pipeline's
// forward pixel shader discards everything below 0.3, so no grass would be visible. Scale the alpha before it's set.
constexpr UINT_PTR GRASS_COLOUR_CALLBACK = 0x5DAC10;
constexpr UINT_PTR GRASS_CALLBACK_SITE = 0x5DAEC7;
constexpr UINT_PTR RENDER2D_CALL_SITE = 0x53EB12;

// The operand after each A1 is a linked address, so bytes rather than containment tell functions apart.
static constexpr unsigned char g_aExpectedGrassCallbackPrologue[] =
{
    0x8B, 0x44, 0x24, 0x08,         // mov eax, [esp+8]
    0x8B, 0x08,                     // mov ecx, [eax]
    0xA1, 0xC0, 0x2D, 0xC0, 0x00,   // mov eax, ds:[0C02DC0h]
    0x56,                           // push esi
    0x8B, 0x74, 0x24, 0x08          // mov esi, [esp+8]
};

static constexpr unsigned char g_aExpectedRender2dPrologue[] =
{
    0xA1, 0x24, 0x7B, 0xC9, 0x00,   // mov eax, ds:[0C97B24h]
    0x83, 0xEC, 0x18,               // sub esp, 18h
    0x6A, 0x00,                     // push 0
    0x6A, 0x06,                     // push 6
    0xFF, 0x50, 0x20,               // call dword ptr [eax+20h]
    0x8B                            // mov, the next instruction, cut at its first byte
};

static_assert( sizeof( g_aExpectedGrassCallbackPrologue ) == 16 &&
                   sizeof( g_aExpectedRender2dPrologue ) == 16,
               "the install time checks compare sixteen bytes of each target" );

using TRender2dPass = void ( __cdecl * )( void );

static RpMaterial *SetGrassMaterialColour( RpMaterial *material, void *data )
{
    const float alphaScale = 6.0f;
    RwRGBA colour = *static_cast<RwRGBA *>( data );
    colour.alpha  = static_cast<RwUInt8>( min( colour.alpha * alphaScale, 255.0f ) );
    return ( (RpMaterial * (__cdecl *)( RpMaterial *, void * ))GRASS_COLOUR_CALLBACK )( material, &colour );
}

enum class ERender2dRefusal
{
    CallSiteOutsideImage,
    CallSiteUnreadable,
    CallSiteNotACall,
    TargetOutsideImage,
    TargetNotExecutable,
    TargetUnreadable,
    TargetNotTheFunction,
    Count
};

static void ReportRender2dRefusal( ERender2dRefusal refusal, const char *reason )
{
    static bool s_aReported[static_cast<int>( ERender2dRefusal::Count )] = {};
    const int index = static_cast<int>( refusal );
    if ( !g_pDebug || !reason || index < 0 || index >= static_cast<int>( ERender2dRefusal::Count ) )
        return;
    if ( s_aReported[index] )
        return;
    s_aReported[index] = true;
    char report[256];
    _snprintf_s( report, sizeof( report ), _TRUNCATE, "Render2d: %s, so the 2D pass is skipped", reason );
    g_pDebug->printError( report );
}

static TRender2dPass FindRender2dPass()
{
    if ( !IsInHostImage( reinterpret_cast<const void *>( RENDER2D_CALL_SITE ), REL32_BRANCH_LENGTH ) )
    {
        ReportRender2dRefusal( ERender2dRefusal::CallSiteOutsideImage,
                               "the call site is not inside this executable's own image" );
        return nullptr;
    }
    if ( !IsReadableRange( reinterpret_cast<const void *>( RENDER2D_CALL_SITE ), REL32_BRANCH_LENGTH ) )
    {
        ReportRender2dRefusal( ERender2dRefusal::CallSiteUnreadable,
                               "the call site is not readable bytes" );
        return nullptr;
    }
    if ( *reinterpret_cast<const unsigned char *>( RENDER2D_CALL_SITE ) != REL32_CALL_OPCODE )
    {
        ReportRender2dRefusal( ERender2dRefusal::CallSiteNotACall,
                               "the call this pass is reached through is not where this build expects it" );
        return nullptr;
    }
    // Read through the helper the writers use; a backward call needs an unsigned sum to reach its target.
    const uint32_t render2dDisplacement = ReadRel32Displacement( RENDER2D_CALL_SITE );
    const UINT_PTR render2dTarget = RENDER2D_CALL_SITE + REL32_BRANCH_LENGTH + render2dDisplacement;
    // Position, executability, readability then identity; a page this process may run is not always one it may
    // load from.
    if ( !IsInHostImage( reinterpret_cast<const void *>( render2dTarget ), sizeof( g_aExpectedRender2dPrologue ) ) )
    {
        ReportRender2dRefusal( ERender2dRefusal::TargetOutsideImage,
                               "the displacement at the call site decodes to an address outside this executable's own "
                               "image" );
        return nullptr;
    }
    if ( !IsExecutableRange( reinterpret_cast<const void *>( render2dTarget ),
                             sizeof( g_aExpectedRender2dPrologue ) ) )
    {
        ReportRender2dRefusal( ERender2dRefusal::TargetNotExecutable,
                               "the displacement at the call site decodes to an address this process cannot execute" );
        return nullptr;
    }
    if ( !IsReadableRange( reinterpret_cast<const void *>( render2dTarget ),
                           sizeof( g_aExpectedRender2dPrologue ) ) )
    {
        ReportRender2dRefusal( ERender2dRefusal::TargetUnreadable,
                               "the displacement at the call site decodes to an address this process cannot read, so "
                               "the bytes naming it were not compared" );
        return nullptr;
    }
    if ( !HoldsExpectedBytes( render2dTarget, g_aExpectedRender2dPrologue, sizeof( g_aExpectedRender2dPrologue ) ) )
    {
        ReportRender2dRefusal( ERender2dRefusal::TargetNotTheFunction,
                               "the displacement at the call site decodes to a function this build was not made for" );
        return nullptr;
    }
    return reinterpret_cast<TRender2dPass>( render2dTarget );
}

// A const function local static, initialised once, so nothing can re-point the 2D pass after the install.
static TRender2dPass Render2dPass()
{
    static TRender2dPass const pfnRender2dPass = FindRender2dPass();
    return pfnRender2dPass;
}

// Names the first differing byte past the ones the gate verifies, once and as an error, because the redirect
// goes ahead regardless.
static void ReportIdleTailDivergence()
{
    if ( !g_pDebug )
        return;
    if ( !IsReadableRange( reinterpret_cast<const void *>( IDLE_FUNCTION_ADDRESS + IDLE_VERIFIED_LENGTH ),
                           IDLE_PROLOGUE_LENGTH - IDLE_VERIFIED_LENGTH ) )
        return;
    for ( SIZE_T index = IDLE_VERIFIED_LENGTH; index < IDLE_PROLOGUE_LENGTH; ++index )
    {
        if ( *reinterpret_cast<const unsigned char *>( IDLE_FUNCTION_ADDRESS + index ) ==
             g_aExpectedIdlePrologue[index] )
            continue;
        char report[256];
        _snprintf_s( report, sizeof( report ), _TRUNCATE,
                     "Idle hook: the idle function's entry differs from the pattern this hook was built for at byte "
                     "offset %u and possibly beyond, past the point the trampoline reads, so the redirect still goes "
                     "ahead", static_cast<unsigned int>( index ) );
        g_pDebug->printError( report );
        return;
    }
}

// Settled once, since the store runs every frame.
static bool g_bDayNightBalanceStorable = false;

void CSAIdleHook::Patch()
{
    (void)Render2dPass();
    // Settled before every return below, so a refusal at one of the sites that follow doesnt take the weight down
    // with it.
    g_bDayNightBalanceStorable =
        IsInHostImage( reinterpret_cast<const void *>( GAME_DAY_NIGHT_BALANCE_ADDR ), sizeof( float ) ) &&
        IsWritableRange( reinterpret_cast<const void *>( GAME_DAY_NIGHT_BALANCE_ADDR ), sizeof( float ) );
    if ( !g_bDayNightBalanceStorable )
        g_pDebug->printError( "Day and night blend: the weight the building renderer reads is not writable memory "
                              "inside this executable's own image, so it was left at whatever the game set and "
                              "building colours, particles and exposure will not follow the clock" );
    // Operand of push 5DAC10h in CGrassRenderer::DrawTriPlants. Safe only where the site still holds that push.
    if ( !IsInHostImage( reinterpret_cast<const void *>( GRASS_CALLBACK_SITE ), PUSH_IMM32_LENGTH ) ||
         !IsReadableRange( reinterpret_cast<const void *>( GRASS_CALLBACK_SITE ), PUSH_IMM32_LENGTH ) )
        g_pDebug->printError( "Grass hook: the site the plant colour callback belongs at is not readable bytes "
                              "inside this executable's own image, so the game keeps the callback it set up "
                              "itself" );
    else
    {
        const unsigned char grassOpcode =
            *reinterpret_cast<const unsigned char *>( GRASS_CALLBACK_SITE );
        // Copied, not cast: those bytes are the games instruction, so no UINT_PTR object lives there.
        UINT_PTR grassOperand;
        static_assert( PUSH_IMM32_OPCODE_LENGTH + static_cast<int>( sizeof( grassOperand ) ) == PUSH_IMM32_LENGTH,
                       "the operand is four bytes wide by the opcode, and the copy and the write below both use the "
                       "width of this variable, so a target whose pointer is wider would overwrite the instruction "
                       "after the operand" );
        memcpy( &grassOperand, reinterpret_cast<const void *>( GRASS_CALLBACK_SITE + PUSH_IMM32_OPCODE_LENGTH ),
                sizeof( grassOperand ) );
        if ( grassOpcode != PUSH_IMM32_OPCODE )
            g_pDebug->printError( "Grass hook: the site the plant colour callback belongs at does not hold the push "
                                  "this hook replaces, so the game keeps the callback it set up itself" );
        else if ( grassOperand == GRASS_COLOUR_CALLBACK )
        {
            if ( !IsInHostImage( reinterpret_cast<const void *>( GRASS_COLOUR_CALLBACK ),
                                 sizeof( g_aExpectedGrassCallbackPrologue ) ) ||
                 !IsExecutableRange( reinterpret_cast<const void *>( GRASS_COLOUR_CALLBACK ),
                                     sizeof( g_aExpectedGrassCallbackPrologue ) ) ||
                 !HoldsExpectedBytes( GRASS_COLOUR_CALLBACK, g_aExpectedGrassCallbackPrologue,
                                      sizeof( g_aExpectedGrassCallbackPrologue ) ) )
                g_pDebug->printError( "Grass hook: the plant colour callback is not the function this hook was built "
                                      "for at an address this process can execute inside its own image, so the game "
                                      "keeps the callback it set up itself" );
            else if ( !SetPointer( GRASS_CALLBACK_SITE + PUSH_IMM32_OPCODE_LENGTH,
                                   reinterpret_cast<void *>( SetGrassMaterialColour ) ) )
                g_pDebug->printError( "Grass hook: the site the plant colour callback is written to could not be "
                                      "made writable, so the game keeps the callback it set up itself" );
        }
        else if ( grassOperand != reinterpret_cast<UINT_PTR>( &SetGrassMaterialColour ) )
            g_pDebug->printError( "Grass hook: the plant colour callback is not where this hook expects it, so the "
                                  "game keeps the callback it set up itself" );
    }

    // Stronger than the inner call's gate below: this address takes the branch and the trampoline re-executes its
    // bytes.
    if ( !IsInHostImage( reinterpret_cast<const void *>( IDLE_FUNCTION_ADDRESS ), IDLE_PROLOGUE_LENGTH ) ||
         !IsReadableRange( reinterpret_cast<const void *>( IDLE_FUNCTION_ADDRESS ), IDLE_PROLOGUE_LENGTH ) ||
         !IsExecutableRange( reinterpret_cast<const void *>( IDLE_FUNCTION_ADDRESS ), IDLE_PROLOGUE_LENGTH ) )
    {
        g_pDebug->printError( "Idle hook: the idle function's entry is not readable, executable bytes inside this "
                              "executable's own image, so it was left alone and this plugin's frame pipeline will not "
                              "run" );
        return;
    }
    const unsigned char *pIdle =
        reinterpret_cast<const unsigned char *>( IDLE_FUNCTION_ADDRESS );
    const EBytesMatch idlePrefix =
        CompareExpectedBytes( IDLE_FUNCTION_ADDRESS, g_aExpectedIdlePrologue, IDLE_VERIFIED_LENGTH );
    if ( idlePrefix == EBytesMatch::Unreadable )
    {
        g_pDebug->printError( "Idle hook: the idle function's entry stopped being readable memory between the test "
                              "above and this comparison, so it was left alone and this plugin's frame pipeline will "
                              "not run" );
        return;
    }
    if ( idlePrefix == EBytesMatch::Mismatch )
    {
        if ( HoldsOwnBranch( IDLE_FUNCTION_ADDRESS, REL32_JUMP_OPCODE, reinterpret_cast<void *>( &Idle ) ) )
            return;
        if ( pIdle[0] == REL32_JUMP_OPCODE )
        {
            g_pDebug->printError( "Idle hook: the idle function's entry already jumps somewhere else, so another "
                                  "hook or another copy of this plugin owns it and this copy's frame pipeline "
                                  "will not run" );
            return;
        }
        g_pDebug->printError( "Idle hook: this executable's idle function is not the one the hook was built "
                              "for, so it was left alone and this plugin's frame pipeline will not run" );
        return;
    }
    // Past the verified window a difference is reported and the redirect goes ahead; those bytes carry no address.
    else if ( CompareExpectedBytes( IDLE_FUNCTION_ADDRESS, g_aExpectedIdlePrologue, IDLE_PROLOGUE_LENGTH ) !=
              EBytesMatch::Match )
        ReportIdleTailDivergence();

    if ( !IsInHostImage( reinterpret_cast<const void *>( IDLE_INNER_CALL ),
                         sizeof( g_aExpectedInnerCallPrologue ) ) ||
         !IsExecutableRange( reinterpret_cast<const void *>( IDLE_INNER_CALL ),
                             sizeof( g_aExpectedInnerCallPrologue ) ) ||
         !HoldsExpectedBytes( IDLE_INNER_CALL, g_aExpectedInnerCallPrologue,
                              sizeof( g_aExpectedInnerCallPrologue ) ) )
    {
        g_pDebug->printError( "Idle hook: the call the trampoline re-enters is not the function this hook was built "
                              "for at an address this process can execute inside its own image, so it was left "
                              "alone and this plugin's frame pipeline will not run" );
        return;
    }

    // Checked rather than the bool, since a write the processor wasnt told about would read as a clean install.
    switch ( RedirectJumpChecked( IDLE_FUNCTION_ADDRESS, reinterpret_cast<void *>( Idle ) ) )
    {
    case EWriteResult::Failed:
        g_pDebug->printError( "Idle hook: the idle function's entry could not be given this hook's jump, so this "
                              "plugin's frame pipeline will not run" );
        break;
    case EWriteResult::WrittenNotClean:
        g_pDebug->printError( "Idle hook: the idle function's entry holds this hook's jump but the processor was not "
                              "told those bytes changed, so it may keep running the unpatched entry and this "
                              "plugin's frame pipeline may not run" );
        break;
    case EWriteResult::Clean:
        break;
    }
}

// Depth inside the entry, per thread. A host that owns the message loop pumps messages straight back here.
static thread_local unsigned int g_uiIdleDepth = 0;

struct SIdleDepthScope
{
    SIdleDepthScope()
    {
        ++g_uiIdleDepth;
    }
    ~SIdleDepthScope()
    {
        --g_uiIdleDepth;
    }

    SIdleDepthScope( const SIdleDepthScope & ) = delete;
    SIdleDepthScope & operator=( const SIdleDepthScope & ) = delete;
};

// Whether the frame this thread is in runs the hook pipeline, read once because the menu can flip it mid frame.
static thread_local bool g_bFrameRunsHook = false;

struct SFrameModeScope
{
    explicit SFrameModeScope( bool bRunsHook )
        : m_bPrevious( g_bFrameRunsHook )
    {
        g_bFrameRunsHook = bRunsHook;
    }
    ~SFrameModeScope()
    {
        g_bFrameRunsHook = m_bPrevious;
    }

    SFrameModeScope( const SFrameModeScope & ) = delete;
    SFrameModeScope & operator=( const SFrameModeScope & ) = delete;

private:
    bool m_bPrevious;
};

static std::atomic<DWORD> g_dwFrameThread( 0 );

// Recorded rather than reported, since CDebug is not thread safe; one atomic for both ids keeps the pairing.
static std::atomic<unsigned long long> g_ullClaimRefusal( 0 );
static std::atomic<unsigned long> g_ulClaimRefusals( 0 );

struct SFrameClaimScope
{
    SFrameClaimScope()
    {
        m_dwOwner = GetCurrentThreadId();
        DWORD dwExpected = 0;
        m_bClaimed =
            g_dwFrameThread.compare_exchange_strong( dwExpected, m_dwOwner, std::memory_order_acq_rel );
        // On failure the exchange reports what it saw, which is the thread to name. Reading the atomic again would
        // race.
        m_dwObserved = dwExpected;
    }
    ~SFrameClaimScope()
    {
        if ( !m_bClaimed )
            return;
        DWORD dwExpected = m_dwOwner;
        g_dwFrameThread.compare_exchange_strong( dwExpected, 0, std::memory_order_acq_rel );
    }
    bool Claimed() const { return m_bClaimed; }
    DWORD ObservedOwner() const { return m_dwObserved; }

    SFrameClaimScope( const SFrameClaimScope & ) = delete;
    SFrameClaimScope & operator=( const SFrameClaimScope & ) = delete;

private:
    DWORD m_dwOwner = 0;
    DWORD m_dwObserved = 0;
    bool m_bClaimed = false;
};

// Each exit that abandons a frame before anything is drawn names itself.
static void ReportIdleFrameAbandoned( const char *reason )
{
    if ( !g_pDebug || reason == nullptr )
        return;
    char report[256];
    _snprintf_s( report, sizeof( report ), _TRUNCATE,
                 "Idle hook: %s, so nothing was drawn or presented this frame", reason );
    g_pDebug->printError( report );
}

// GetScreenFadeStatus: 0 clear, 1 fading, 2 fully faded.
constexpr int FADE_STATUS_FULLY_FADED = 2;

// DoFade fades in only while StillToFadeOut is set and FadeOutTime has passed since FadeTimer was stamped.
constexpr UINT_PTR TIMER_FUNCTION          = 0xB7CB28;
constexpr UINT_PTR GAME_JUST_LOADED        = 0xC16EDC;
constexpr UINT_PTR GAME_STILL_TO_FADE_OUT  = 0xC16EDD;
constexpr UINT_PTR GAME_FADE_START_STAMP   = 0xC16EE0;
constexpr UINT_PTR GAME_FADE_DELAY         = 0x8D2BD4;  // FadeOutTime, the fade-in delay, 1750 ms

// Probe a fixed game address before reading it, so a wrong one costs a skipped report and not a fault on the game frame.
static bool IsGameSampleReadable( UINT_PTR address, SIZE_T size )
{
    return IsInHostImage( reinterpret_cast<const void *>( address ), size ) &&
           IsReadableRange( reinterpret_cast<const void *>( address ), size );
}

// Names a full fade with the menu closed, once every interval, so a short fade stays silent and a stuck one is nameable.
static void ReportFullFadeFrame( bool bMenuActive, int iFadeStatus )
{
    constexpr unsigned long FULL_FADE_REPORT_INTERVAL = 600;
    // Every write differs, so CDebug's dedupe never holds it back; the cap keeps a stuck fade from growing the log.
    constexpr unsigned long FULL_FADE_REPORT_LIMIT = 3;
    static unsigned long s_ulFullFadeFrames = 0;
    static unsigned long s_ulFullFadeReports = 0;
    static int s_iPauseAtStart = 0;

    if ( bMenuActive || iFadeStatus != FADE_STATUS_FULLY_FADED )
    {
        s_ulFullFadeFrames  = 0;
        s_ulFullFadeReports = 0;
        return;
    }
    // Taken before the pause decision below, so this is the value the state started with.
    if ( s_ulFullFadeFrames == 0 )
        s_iPauseAtStart = CTimer::m_UserPause ? 1 : 0;
    ++s_ulFullFadeFrames;
    if ( s_ulFullFadeReports >= FULL_FADE_REPORT_LIMIT || !g_pDebug )
        return;
    if ( s_ulFullFadeFrames == 0 || s_ulFullFadeFrames % FULL_FADE_REPORT_INTERVAL != 0 )
        return;
    // Probed before the first read and outside the budget, so a wrong address costs one line, not a fault or a report.
    if ( !IsGameSampleReadable( TIMER_FUNCTION, sizeof( unsigned int ) ) ||
         !IsGameSampleReadable( GAME_FADE_START_STAMP, sizeof( unsigned int ) ) ||
         !IsGameSampleReadable( GAME_FADE_DELAY, sizeof( unsigned int ) ) ||
         !IsGameSampleReadable( GAME_JUST_LOADED, sizeof( unsigned char ) ) ||
         !IsGameSampleReadable( GAME_STILL_TO_FADE_OUT, sizeof( unsigned char ) ) )
    {
        static bool s_bUnreadableSampleReported = false;
        if ( !s_bUnreadableSampleReported && g_pDebug )
        {
            s_bUnreadableSampleReported = true;
            g_pDebug->printError( "Idle hook: the menu is closed and the camera reports a full screen fade, but the "
                                  "game state this report reads is not readable memory inside this executable's own "
                                  "image, so the fade was not sampled; the frame is still drawn and paced" );
        }
        return;
    }
    // Both pause flags are clear here, so the fade state and its clock are what name the cause.
    const float fAlpha    = TheCamera.m_fFadeAlpha;
    const float fDuration = TheCamera.m_fFadeDuration;
    const float fTimeStep = CTimer::ms_fTimeStep;
    // ProcessFade tests this byte against 1, not non-zero, so the report matches it.
    const unsigned char uchFadeRunning = *(const unsigned char *)&TheCamera.m_bFading;
    // One read each, so this unsigned subtraction matches DoFade's and cant straddle two clock ticks.
    const unsigned long ulStamp   = *(const unsigned int *)GAME_FADE_START_STAMP;
    const unsigned long ulClock   = CTimer::m_snTimeInMilliseconds;
    const unsigned long ulDelay   = *(const unsigned int *)GAME_FADE_DELAY;
    const unsigned long ulElapsed = ulClock - ulStamp;
    char report[1280];
    _snprintf_s( report, sizeof( report ), _TRUNCATE,
                 "Idle hook: the menu is closed and the camera reports screen fade status %d, so the world was drawn "
                 "under the fade overlay for %lu frames; the fade alpha is %.0f of 255, the fade is %s, its "
                 "direction is %d and its duration is %.0f ms; StillToFadeOut is %d, JustLoadedDontFadeInYet is %d, "
                 "their start stamp is %lu, the millisecond clock now reads %lu so %lu ms have passed against the "
                 "%lu ms delay that gates the fade-in, which %s; the fade start reading is %lu; CTimer::m_UserPause "
                 "was %d when the state began and is %d now, m_CodePause is %d, ms_fTimeStep is %.0f thousandths, the "
                 "timer function is %s, CTimer::Update has stepped its counter %lu times, its non-clipped clock "
                 "accumulator reads %lu, its pause-mode accumulator reads %lu and the cycle count "
                 "GetCyclesPerMillisecond reports is %lu, and CDraw::FadeValue is %d; at most %lu reports are "
                 "made for one run of this state, after which it stays silent",
                 iFadeStatus, s_ulFullFadeFrames,
                 static_cast<double>( fAlpha ),
                 uchFadeRunning == 1 ? "running" : "not running",
                 static_cast<int>( TheCamera.m_nFadeInOutFlag ),
                 static_cast<double>( fDuration * 1000.0f ),
                 *(const unsigned char *)GAME_STILL_TO_FADE_OUT ? 1 : 0,
                 *(const unsigned char *)GAME_JUST_LOADED ? 1 : 0,
                 ulStamp, ulClock, ulElapsed, ulDelay,
                 *(const unsigned char *)GAME_STILL_TO_FADE_OUT
                     ? ( ulElapsed > ulDelay ? "is due" : "cannot start yet" )
                     : "has no pending fade to start",
                 static_cast<unsigned long>( TheCamera.m_nFadeStartTime ),
                 s_iPauseAtStart, CTimer::m_UserPause ? 1 : 0, CTimer::m_CodePause ? 1 : 0,
                 static_cast<double>( fTimeStep * 1000.0f ),
                 *(const unsigned int *)TIMER_FUNCTION != 0 ? "set" : "null",
                 static_cast<unsigned long>( CTimer::m_FrameCounter ),
                 static_cast<unsigned long>( CTimer::m_snTimeInMillisecondsNonClipped ),
                 static_cast<unsigned long>( CTimer::m_snTimeInMillisecondsPauseMode ),
                 static_cast<unsigned long>( static_cast<unsigned int>( CTimer::GetCyclesPerMillisecond() ) ),
                 static_cast<int>( CDraw::FadeValue ), FULL_FADE_REPORT_LIMIT );
    g_pDebug->printError( report );
    // Counted after the write, so a swallowed report doesnt spend one of the three.
    ++s_ulFullFadeReports;
}

// Set while the bar's own pause stands in CTimer::m_UserPause; only touched inside a claimed frame.
static bool s_bGuiPauseHeld = false;

constexpr size_t WIDEST_32_BIT_DECIMAL = 11;

// Counted from the text, so the allowance cant drift from the message it belongs to.
static constexpr size_t CountConversions( const char *format, size_t length )
{
    size_t conversions = 0;
    for ( size_t index = 0; index < length; ++index )
        conversions += format[index] == '%' ? 1 : 0;
    return conversions;
}

// Named so the buffer is checked against it at build time; a clipped message reads as if it ended there.
static constexpr char IDLE_DROPPED_CALL_REPORT_FORMAT[] =
    "Idle hook: the host called the idle entry from inside this plugin's frame at depth %u on "
    "thread %lu, so the call was dropped rather than running a second complete frame%s; drop %lu "
    "on this thread";

static constexpr char IDLE_PASSTHROUGH_NOTE[] =
    " (the surrounding frame is a pass-through, so the hook is off)";

static constexpr char IDLE_CLAIM_REFUSAL_REPORT_FORMAT[] =
    "Idle hook: the idle entry was called on thread %lu while thread %lu was inside a frame, so "
    "that call was refused rather than running two frames at once; %lu refused since the last "
    "report, the most recent pair named here";

void CSAIdleHook::Idle( void *Data )
{
    // A call inside a frame came from a message the host pumped mid frame, and _Idle runs a whole CGame::Idle,
    // so it is dropped.
    if ( g_uiIdleDepth != 0 )
    {
        constexpr unsigned long IDLE_DROP_REPORT_INTERVAL = 600;
        static thread_local unsigned long s_aulDropped[2] = { 0, 0 };
        unsigned long &ulDropped = s_aulDropped[g_bFrameRunsHook ? 1 : 0];
        // Unsigned overflow wraps to zero, which is not a drop count, so zero is skipped.
        ++ulDropped;
        if ( ulDropped != 0 && ( ulDropped == 1 || ulDropped % IDLE_DROP_REPORT_INTERVAL == 0 ) )
        {
            char report[384];
            // Every conversion is charged a full integer, so this overstates the message and a longer one fails here.
            static_assert( sizeof( report ) >= sizeof( IDLE_DROPPED_CALL_REPORT_FORMAT ) - 1 +
                                                   CountConversions( IDLE_DROPPED_CALL_REPORT_FORMAT,
                                                                     sizeof( IDLE_DROPPED_CALL_REPORT_FORMAT ) - 1 ) *
                                                       WIDEST_32_BIT_DECIMAL +
                                                   sizeof( IDLE_PASSTHROUGH_NOTE ) - 1,
                           "the dropped call report has to fit the buffer it is written to, or its end is lost" );
            _snprintf_s( report, sizeof( report ), _TRUNCATE, IDLE_DROPPED_CALL_REPORT_FORMAT,
                         g_uiIdleDepth, GetCurrentThreadId(),
                         g_bFrameRunsHook ? "" : IDLE_PASSTHROUGH_NOTE,
                         ulDropped );
            if ( g_pDebug )
                g_pDebug->printError( report );
        }
        return;
    }

    // What the depth above cannot see. Declared first, so the claim releases after the frame's own scopes.
    SFrameClaimScope frameClaim;
    if ( !frameClaim.Claimed() )
    {
        // Recorded only, because this is not the frame thread and CDebug is not thread safe.
        g_ullClaimRefusal.store( ( static_cast<unsigned long long>( frameClaim.ObservedOwner() ) << 32 ) |
                                 static_cast<unsigned long long>( GetCurrentThreadId() ),
                                 std::memory_order_relaxed );
        // Release, so a frame thread that drains this count also sees the pair stored above it.
        g_ulClaimRefusals.fetch_add( 1, std::memory_order_release );
        return;
    }
    // Drained only when there is somewhere to write it.
    if ( g_pDebug )
    {
        const unsigned long ulRefusals = g_ulClaimRefusals.exchange( 0, std::memory_order_acquire );
        if ( ulRefusals != 0 )
        {
            const unsigned long long refusal = g_ullClaimRefusal.load( std::memory_order_relaxed );
            char report[320];
            // As above: fail the build if the text or conversion count outgrows the buffer.
            static_assert( sizeof( report ) >= sizeof( IDLE_CLAIM_REFUSAL_REPORT_FORMAT ) - 1 +
                                                   CountConversions( IDLE_CLAIM_REFUSAL_REPORT_FORMAT,
                                                                     sizeof( IDLE_CLAIM_REFUSAL_REPORT_FORMAT ) - 1 ) *
                                                       WIDEST_32_BIT_DECIMAL,
                           "the claim refusal report has to fit the buffer it is written to, or its end is lost" );
            _snprintf_s( report, sizeof( report ), _TRUNCATE, IDLE_CLAIM_REFUSAL_REPORT_FORMAT,
                         static_cast<unsigned long>( refusal & 0xFFFFFFFFULL ),
                         static_cast<unsigned long>( refusal >> 32 ), ulRefusals );
            g_pDebug->printError( report );
        }
    }

    // Held for the whole claimed frame, so a call arriving from inside it reads as the frame it is inside.
    SIdleDepthScope frame;
    const bool bHookActive = gDebugSettings.GetToggleField( "UseIdleHook" );
    SFrameModeScope frameMode( bHookActive );
    SettingsHolder::Instance()->InitGUI();
    if ( !bHookActive )
    {
        // Returns before the pause is decided below, so a pause the bar took would outlive the hook being switched
        // off.
        if ( s_bGuiPauseHeld )
        {
            s_bGuiPauseHeld = false;
            if ( !FrontEndMenuManager.m_bMenuActive )
                CTimer::m_UserPause = false;
        }
        _Idle( Data );
        SettingsHolder::Instance()->DrawGUI();
        return;
    }
    // Refused before the first mutation, since the updates below advance the clock and world for a frame never drawn.
    if ( !Data )
    {
        ReportIdleFrameAbandoned( "the idle entry was called with no data argument" );
        return;
    }
    SettingsHolder::Instance()->ReloadShadersIfRequired();
    // Update timers
    TimeUpdate();
    // Init 2D stuff per frame.
    InitPerFrame2D();
    // Update game processes
    GameUpdate();
    // Update lighting
    LightUpdate();

    CRwD3D1XEngine* dxEngine = (CRwD3D1XEngine*)g_pRwCustomEngine;

    // RenderHUD is the only caller of RsCameraShowRaster, so abandoning the frame here also abandons the present.
    PrepareRwCamera();
    if ( !RsCameraBeginUpdate( Scene.m_pRwCamera ) )
    {
        ReportIdleFrameAbandoned( "the camera refused to begin an update" );
        return;
    }
    // This drain stays after the camera begins updating, since that call raises the size flag; all six clear
    // together.
    g_pDeferredRenderer->QueueTextureReload();
    CVolumetricLighting::QueueTextureReload();
    CCloudRendering::QueueTextureReload();
    CAmbientOcclusion::QueueTextureReload();
    CTemporalAA::QueueTextureReload();
    if ( dxEngine->m_bScreenSizeChanged || g_pDeferredRenderer->m_pShadowRenderer->m_bRequiresReloading ||
         g_pDeferredRenderer->m_bRequiresReloading ||
         CVolumetricLighting::m_bRequiresReloading ||
         CAmbientOcclusion::m_bRequiresReloading ||
         CCloudRendering::m_bRequiresReloading )
    {
        dxEngine->ReloadTextures();
        dxEngine->m_bScreenSizeChanged = false;
        g_pDeferredRenderer->m_pShadowRenderer->m_bRequiresReloading = false;
        g_pDeferredRenderer->m_bRequiresReloading = false;
        CVolumetricLighting::m_bRequiresReloading                    = false;
        CAmbientOcclusion::m_bRequiresReloading                      = false;
        CCloudRendering::m_bRequiresReloading                        = false;
    }
    if ( !FrontEndMenuManager.m_bMenuActive )
        PrepareRenderStuff();
    else if ( SettingsHolder::Instance()->IsGUIEnabled() )
        SettingsHolder::Instance()->DisableGUI();
    // Read once; the game owns this flag and can change it between two reads.
    const bool bMenuActive  = FrontEndMenuManager.m_bMenuActive;
    const int  iFadeStatus  = TheCamera.GetScreenFadeStatus();
    // DoFade draws the fade as an overlay afterwards, so the world renders under it; a full fade is no reason to skip it.
    const bool bDrawFrame = !bMenuActive;
    // Fade clause stays: a pause stops the clock the fade advances on, and DoFade bails on the same flag before
    // drawing its overlay, which would leave the world unblackened.
    const bool bGuiWantsPause = bDrawFrame && iFadeStatus != FADE_STATUS_FULLY_FADED &&
                                SettingsHolder::Instance()->IsGUIEnabled();
    ReportFullFadeFrame( bMenuActive, iFadeStatus );
    // Written when it differs and cleared only when this bar holds it; releasing every frame would drop the pause a
    // menu's message loop takes.
    const bool bWeHoldPause = s_bGuiPauseHeld && CTimer::m_UserPause;
    if ( bGuiWantsPause != CTimer::m_UserPause &&
         ( bGuiWantsPause || ( bWeHoldPause && !bMenuActive ) ) )
    {
        CTimer::m_UserPause = bGuiWantsPause;
        s_bGuiPauseHeld     = bGuiWantsPause;
    }
    else
        s_bGuiPauseHeld = bWeHoldPause;
    if ( bDrawFrame )
    {
        RenderInGame();
        SettingsHolder::Instance()->DrawGUI();
        DefinedState2d();
        if ( SettingsHolder::Instance()->IsGUIEnabled() )
        {
            POINT mousePos{};
            const HWND hWnd = dxEngine->getRenderer()->getHWND();
            if ( hWnd && GetCursorPos( &mousePos ) && ScreenToClient( hWnd, &mousePos ) )
            {
                //g_pRwCustomEngine->RenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, TRUE);
                FrontEndMenuManager.m_apTextures[23].SetRenderState();
                FrontEndMenuManager.m_apTextures[23].DrawTxRect(
                    { (float)mousePos.x, (float)mousePos.y,
                    (float)mousePos.x + 10.0f, (float)mousePos.y + 16.0f },
                    { 255,255,255,255 } );
            }
        }
    }
    RenderHUD();
}

void CSAIdleHook::UpdateShadowDNBalance()
{
    // TODO: make times adjustable perhaps?
    float currentMinutes = ( CClock::ms_nGameClockMinutes + 60.0f * CClock::ms_nGameClockHours ) + CClock::ms_nGameClockSeconds / 60.0f;
    // Worked out first and stored once below, so every path leaves both values describing the same
    // reading of the clock.
    float fBalance;
    // if time is less than 7 am then it's night
    if ( currentMinutes < 360.0 )
        fBalance = 1.0f;
    else if ( currentMinutes < 420.0 )
        fBalance = ( 420.0f - currentMinutes ) / 60.0f;
    // if time is between 7 am and 7 pm than it's day
    else if ( currentMinutes < 1080.0 )
        fBalance = 0.0f;
    // else it's night
    else if ( currentMinutes >= 1140.0 )
        fBalance = 1.0f;
    else
        fBalance = 1.0f - ( 1140.0f - currentMinutes ) / 60.0f;

    m_fShadowDNBalance = fBalance;
    // The game's day/night blend weight is written by its custom building renderer update, and this loop
    // replaces that update, so nothing else writes it while the game runs. Building colours, particles,
    // skidmarks and the exposure of every pixel read it, and all of them would otherwise see the value it
    // was initialised to, which is full night, for the whole session.
    if ( g_bDayNightBalanceStorable )
        *(float*)( GAME_DAY_NIGHT_BALANCE_ADDR ) = fBalance;
}

// Render one game frame
void CSAIdleHook::RenderInGame()
{
    CPerfTimer shadowTimer( "Shadow Time" );
    CPerfTimer deferredTimer( "Deferred Time" );
    CPerfTimer scanTimer( "Renderlist construction time" );

    // previous world matrices for object motion vectors are per rendered frame
    g_pRenderBuffersMgr->NextFrame();


    DefinedState();
    TheCamera.m_mViewMatrix.Update();
    g_pRwCustomEngine->RenderStateSet( rwRENDERSTATESTENCILENABLE, FALSE );

    RwCameraSetFarClipPlane( Scene.m_pRwCamera, CTimeCycle::m_CurrentColours.m_fFarClip );

    Scene.m_pRwCamera->fogPlane = CTimeCycle::m_CurrentColours.m_fFogStart;

    // Game variables initialization
    const auto sunDirs = reinterpret_cast<RwV3d*>( 0xB7CA50 );		// Sun direction table pointer(sun directions is always the same)
    const auto curr_sun_dir = *reinterpret_cast<int*>( 0xB79FD0 );	// Current sun direction id
    const auto curr_sun_dirvec = &sunDirs[curr_sun_dir];			// Current sun direction vector

    g_pStateMgr->SetSunDir( curr_sun_dirvec, ( CGame::currArea == 0 ? m_fShadowDNBalance : 1.0f ) );
    g_pStateMgr->SetFogStart( CTimeCycle::m_CurrentColours.m_fFogStart );
    g_pStateMgr->SetFogRange( CTimeCycle::m_CurrentColours.m_fFarClip - CTimeCycle::m_CurrentColours.m_fFogStart );
    g_shaderRenderStateBuffer.vSkyLightCol = { CTimeCycle::m_CurrentColours.m_nSkyTopRed / 255.0f,
                                        CTimeCycle::m_CurrentColours.m_nSkyTopGreen / 255.0f,
                                        CTimeCycle::m_CurrentColours.m_nSkyTopBlue / 255.0f,1.0f };
    g_shaderRenderStateBuffer.vHorizonCol = { CTimeCycle::m_CurrentColours.m_nSkyBottomRed / 255.0f,
                                    CTimeCycle::m_CurrentColours.m_nSkyBottomGreen / 255.0f,
                                    CTimeCycle::m_CurrentColours.m_nSkyBottomBlue / 255.0f,1.0f };
    g_shaderRenderStateBuffer.vSunColor = { CTimeCycle::m_CurrentColours.m_nSunCoreRed / 255.0f,
                                    CTimeCycle::m_CurrentColours.m_nSunCoreGreen / 255.0f,
                                    CTimeCycle::m_CurrentColours.m_nSunCoreBlue / 255.0f, 4.5f/*Timecycle->m_fCurrentSpriteBrightness */ };
    g_shaderRenderStateBuffer.vWaterColor = { CTimeCycle::m_CurrentColours.m_fWaterRed / 255.0f,
                                    CTimeCycle::m_CurrentColours.m_fWaterGreen / 255.0f ,
                                    CTimeCycle::m_CurrentColours.m_fWaterBlue / 255.0f,
                                    CTimeCycle::m_CurrentColours.m_fWaterAlpha / 255.0f };
    g_shaderRenderStateBuffer.vGradingColor0 = { CTimeCycle::m_CurrentColours.m_fPostFx1Red / 255.0f,
                                        CTimeCycle::m_CurrentColours.m_fPostFx1Green / 255.0f ,
                                        CTimeCycle::m_CurrentColours.m_fPostFx1Blue / 255.0f,
                                        CTimeCycle::m_CurrentColours.m_fPostFx1Alpha / 255.0f };
    g_shaderRenderStateBuffer.vGradingColor1 = { CTimeCycle::m_CurrentColours.m_fPostFx2Red / 255.0f,
                                        CTimeCycle::m_CurrentColours.m_fPostFx2Green / 255.0f ,
                                        CTimeCycle::m_CurrentColours.m_fPostFx2Blue / 255.0f,
                                        CTimeCycle::m_CurrentColours.m_fPostFx2Alpha / 255.0f };
    g_shaderRenderStateBuffer.fFarClip = Scene.m_pRwCamera->farPlane;

    // First forward pass(clouds, sky etc.)
    RenderForwardBeforeDeferred();

    g_pDeferredRenderer->m_pShadowRenderer->m_bShadowHistoryValid = g_pDeferredRenderer->m_pShadowRenderer->m_bShadowsRendered;
    g_pDeferredRenderer->m_pShadowRenderer->m_bShadowsRendered = false;
    if ( gShadowSettings.GetToggleField( "Enable" ) && !CGame::currArea && ( m_fShadowDNBalance < 1.0 ) )
        PrepareRealTimeShadows( sunDirs[curr_sun_dir] );

    DebugRendering::ResetList();

    //
    drawCallCount = 0;

    // Render custom preprocess effects - shadows and voxel GI(disabled atm)

    m_uiDeferredStage = 5;
    //
    scanTimer.Start();
    CRendererRH::ConstructRenderList();
    scanTimer.Stop();

    // The game's PreRender already runs PreRender for every visible entity, LOD, and off-screen entity with effects;
    // skip CRendererRH::PreRender so entities and point lights are not pre-rendered twice per frame.
    CRenderer::PreRender();
    CWorld::ProcessPedsAfterPreRender();

    RwCameraEndUpdate( Scene.m_pRwCamera );
    g_pDeferredRenderer->RenderToCubemap( RenderForward );

    shadowTimer.Start();

    if ( gShadowSettings.GetToggleField( "Enable" ) && !CGame::currArea && m_fShadowDNBalance < 1.0 ) // Render shadows only if we are not inside interiors
        RenderRealTimeShadows( sunDirs[curr_sun_dir] );

    shadowTimer.Stop();

    RwCameraBeginUpdate( Scene.m_pRwCamera );
    CTemporalAA::JitterProjMatrix();
    deferredTimer.Start();
    g_pCustomCarFXPipe->ResetAlphaList();
    g_pCustomBuildingPipe->ResetAlphaList();
    g_pCustomBuildingDNPipe->ResetAlphaList();
    // Render scene to geometry buffers.
    g_pDeferredRenderer->RenderToGBuffer( RenderDeferred );

    // Reset renderstates and disable Z-Test
    DefinedState();
    g_pRwCustomEngine->RenderStateSet( rwRENDERSTATEZTESTENABLE, 0 );

    // Render deferred shading
    CD3DRenderer* renderer = static_cast<CRwD3D1XEngine*>( g_pRwCustomEngine )->getRenderer();
    renderer->BeginDebugEvent( L"Deferred composition pass" );
    g_pDeferredRenderer->RenderOutput();
    renderer->EndDebugEvent();
    deferredTimer.Stop();

    // Enable Z-Test and render alpha entities
    g_pRwCustomEngine->RenderStateSet( rwRENDERSTATEZTESTENABLE, 1 );
    m_uiDeferredStage = 0;

    renderer->BeginDebugEvent( L"Forward after deferred" );
    RenderForwardAfterDeferred();
    renderer->EndDebugEvent();

    g_pRwCustomEngine->RenderStateSet( rwRENDERSTATEZTESTENABLE, 0 );
    renderer->BeginDebugEvent( L"Tonemapping pass" );
    g_pDeferredRenderer->RenderTonemappedOutput(); //TODO fix
    renderer->EndDebugEvent();
    //g_pRwCustomEngine->SetRenderTargets( &Scene.m_pRwCamera->frameBuffer,
    //                                     Scene.m_pRwCamera->zBuffer, 1 );
    DebugRendering::Render();
    if ( gDebugSettings.GetToggleField( "DebugRenderTarget" ) &&
         gDebugSettings.DebugRenderTargetNumber < gDebugSettings.DebugRenderTargetList.size() && gDebugSettings.DebugRenderTargetList[gDebugSettings.DebugRenderTargetNumber] != nullptr )
        DebugRendering::RenderRaster( gDebugSettings.DebugRenderTargetList[gDebugSettings.DebugRenderTargetNumber] );

    DefinedState();
    // Restores the game's own 2D pass rather than adding one; the hook replaces CGame::Idle's body, so 0x53EB12 is
    // called here.
    if ( TRender2dPass pfnRender2dPass = Render2dPass() )
        pfnRender2dPass();
    //Render2dStuff();
    // Render preformance counters if required.
    if ( gDebugSettings.GetToggleField( "ShowPreformanceCounters" ) )
    {
        CFont::SetFontStyle( eFontStyle::FONT_SUBTITLES );
        CRGBA color{ 255,255,255,255 };//pi/360
        //CFont::SetAlignment(eFontAlignment::ALIGN_RIGHT);
        //CFont::SetScale(0.5, 0.5);
        CFont::SetColor( color );
        // The framebuffer is null until the first one is made.
        if ( Scene.m_pRwCamera->frameBuffer == nullptr )
            return;
        float FontXPos = Scene.m_pRwCamera->frameBuffer->width - 20.0f;
        float FontYPos = Scene.m_pRwCamera->frameBuffer->height - 20.0f;
        CFont::PrintString( FontXPos, FontYPos - 750.0f, (char*)( "NumBlocksOutsideWorldToBeRendered : " + to_string( CWaterLevel::m_NumBlocksOutsideWorldToBeRendered ) ).c_str() );
        CFont::PrintString( FontXPos, FontYPos - 600.0f, (char*)( "Visible refl entities count: " + to_string( CRendererRH::ms_aVisibleReflectionObjects.size() ) ).c_str() );
        CFont::PrintString( FontXPos, FontYPos - 550.0f, (char*)( "CameraHeading: " + to_string( CRendererRH::ms_fCameraHeading ) ).c_str() );
        CFont::PrintString( FontXPos, FontYPos - 500.0f, (char*)( "Visible Entity count: " + to_string( CRendererRH::ms_nNoOfVisibleEntities ) ).c_str() );
        CFont::PrintString( FontXPos, FontYPos - 450.0f, (char*)( "Draw call count: " + to_string( drawCallCount ) ).c_str() );
        CFont::PrintString( FontXPos, FontYPos - 400.0f, (char*)( "Visible Lod Entity count: " + to_string( CRendererRH::ms_nNoOfVisibleLods ) ).c_str() );
        CFont::PrintString( FontXPos, FontYPos - 350.0f, (char*)( "SC 0 Entity count: " + to_string( CRendererRH::ms_aVisibleShadowCasters[0].size() ) ).c_str() );
        CFont::PrintString( FontXPos, FontYPos - 300.0f, (char*)( "SC 1 Entity count: " + to_string( CRendererRH::ms_aVisibleShadowCasters[1].size() ) ).c_str() );
        CFont::PrintString( FontXPos, FontYPos - 250.0f, (char*)( "SC 2 Entity count: " + to_string( CRendererRH::ms_aVisibleShadowCasters[2].size() ) ).c_str() );
        CFont::PrintString( FontXPos, FontYPos - 200.0f, (char*)( "SC 3 Entity count: " + to_string( CRendererRH::ms_aVisibleShadowCasters[3].size() ) ).c_str() );


        //CFont::PrintString(FontXPos, FontYPos - 150.0f, (char*)("SC 3 Entity count: " + to_string(CRendererRH::ms_aVisibleShadowCasters[3].size())).c_str());

        CFont::PrintString( FontXPos, FontYPos - 150.0f, (char*)scanTimer.GetTimerResult().c_str() );
        CFont::PrintString( FontXPos, FontYPos - 100.0f, (char*)shadowTimer.GetTimerResult().c_str() );
        CFont::PrintString( FontXPos, FontYPos - 50.0f, (char*)deferredTimer.GetTimerResult().c_str() );
    }

}

void CSAIdleHook::RenderHUD()
{
    DefinedState();
    if ( FrontEndMenuManager.m_bMenuActive )
        DrawMenuManagerFrontEnd( &FrontEndMenuManager ); // CMenuManager::DrawFrontEnd
    g_pRwCustomEngine->RenderStateSet( rwRENDERSTATETEXTURERASTER, NULL );

    DoFade();
    Render2dStuffAfterFade();

    DoRWStuffEndOfFrame();
}

void CSAIdleHook::RenderForwardBeforeDeferred()
{
}

void CSAIdleHook::RenderForwardAfterDeferred()
{
    CD3DRenderer* renderer = static_cast<CRwD3D1XEngine*>( g_pRwCustomEngine )->getRenderer();

    renderer->BeginDebugEvent( L"Water rendering pass" );
    g_pDeferredRenderer->SetNormalDepthRaster();
    g_pDeferredRenderer->SetPreviousNonTonemappedFinalRaster();
    g_pDeferredRenderer->m_pShadowRenderer->SetShadowBuffer();
    CWaterLevel::RenderWater();
    g_pDeferredRenderer->m_pShadowRenderer->UnsetShadowBuffer();
    DefinedState();
    renderer->EndDebugEvent();

    renderer->BeginDebugEvent( L"Alpha-blended objects rendering pass" );
    g_pDeferredRenderer->m_pShadowRenderer->SetShadowBuffer();
    g_pDeferredRenderer->m_pReflRenderer->SetCubemap();
    g_pCustomCarFXPipe->RenderAlphaList();
    g_pCustomBuildingPipe->RenderAlphaList();
    g_pCustomBuildingDNPipe->RenderAlphaList();
    BreakManager_c__Render( g_breakMan, 1 );
    g_pDeferredRenderer->m_pShadowRenderer->UnsetShadowBuffer();
    renderer->EndDebugEvent();

    //CPostEffects__m_bDisableAllPostEffect = true;
    // Render effects and 2d stuff
    DefinedState();
    renderer->BeginDebugEvent( L"Effects rendering pass" );
    RenderEffects();
    renderer->EndDebugEvent();
    DefinedState();
    renderer->BeginDebugEvent( L"Grass rendering pass" );
    RenderGrass();
    renderer->EndDebugEvent();
    DefinedState();
}

void CSAIdleHook::RenderDeferred()
{
    CVisibilityPluginsRH::ClearWeaponPedsList();

    //g_pRwCustomEngine->RenderStateSet(rwRENDERSTATECULLMODE, rwCULLMODECULLBACK);
    CRendererRH::RenderRoads();
    CRendererRH::RenderEverythingBarRoads();
    // FadingPass is scoped inside CVisibilityPluginsRH::RenderAlphaAtomic
    CVisibilityPlugins::RenderFadingUnderwaterEntities();
    // Non-fading DrawLast entities stay on the forward alpha path
    BreakManager_c__Render( g_breakMan, 0 );
    // FadingPass is scoped inside CVisibilityPluginsRH::RenderAlphaAtomic
    CVisibilityPlugins::RenderFadingEntities();
    // Non-fading DrawLast entities stay on the forward alpha path
    // BreakManager_c::Render(1) moved to RenderForwardAfterDeferred for translucent breakables
    // CRenderer::RenderFadingInUnderwaterEntities
    CVisibilityPluginsRH::RenderWeaponPedsNoMuzzleFlash();
    //CRenderer::RenderTOBJs();
    //CWaterLevel::RenderSeaBed();
}

void CSAIdleHook::RenderForward()
{
    g_pRwCustomEngine->RenderStateSet( rwRENDERSTATECULLMODE, rwCULLMODECULLNONE );
    CRendererRH::RenderCubemapEntities();
}

void CSAIdleHook::RenderEffects()
{
    //CClouds::Render();
    CBirds::Render();
    //CSkidmarks::Render();
    CRopes::Render();
    //CGlass::Render();
    //CMovingThings::Render();
    CVisibilityPlugins::RenderReallyDrawLastObjects();
    //CCoronas::RenderReflections();
    //CCoronas::RenderSunReflection();
    CD3DRenderer* renderer = static_cast<CRwD3D1XEngine*>( g_pRwCustomEngine )->getRenderer();
    renderer->BeginDebugEvent( L"Coronas rendering" );
    CCoronas::Render();
    renderer->EndDebugEvent();
    //g_pStateMgr->SetAlphaTestEnable(true);
    g_fx.Render( TheCamera.m_pRwCamera, 0 );
    //CWaterCannons::Render();
    //CWaterLevel::RenderWaterFog();
    CClouds::VolumetricCloudsRender();
    CClouds::MovingFogRender();
    /*if (CHeli::NumberOfSearchLights || CTheScripts::NumberOfScriptSearchLights)
    {
        CHeli::Pre_SearchLightCone();
        CHeli::RenderAllHeliSearchLights();
        CTheScripts::RenderAllSearchLights();
        CHeli::Post_SearchLightCone();
    }*/
    CWeaponEffects::Render();
    /*if (CReplay::Mode != 1 && !CPad::GetPad(0)->field_10E)
    {
        v1 = FindPlayerPed(-1);
        CPlayerPed::DrawTriangleForMouseRecruitPed(v1);
    }*/
    CSpecialFX::Render();
    //CVehicleRecording::Render();
    //CPointLights::RenderFogEffect();
    CRenderer::RenderFirstPersonVehicle();
    //g_pRwCustomEngine->RenderStateSet(rwRENDERSTATEZTESTENABLE, 1);
    //g_pRwCustomEngine->RenderStateSet(rwRENDERSTATEZWRITEENABLE, 1);
    //CVisibilityPlugins__RenderWeaponPedsForPC();
    //CPostEffects::Render();
}

void CSAIdleHook::PrepareRealTimeShadows( const RwV3d &sundir )
{
    g_pDebug->printMsg( "Shadow PrePass: begin", 1 );

    auto shadowRenderer = g_pDeferredRenderer->m_pShadowRenderer;

    shadowRenderer->CalculateShadowDistances( Scene.m_pRwCamera->nearPlane, Scene.m_pRwCamera->farPlane );
    shadowRenderer->SelectCascadesToUpdate( sundir );
    // Render cascades
    for ( int i = 0; i < gShadowSettings.ShadowCascadeCount; i++ )
        if ( shadowRenderer->IsCascadeUpdated( i ) )
            shadowRenderer->DirectionalLightTransform( Scene.m_pRwCamera, sundir, i );

    g_pDebug->printMsg( "Shadow PrePass: end", 1 );
}
void CSAIdleHook::RenderRealTimeShadows( const RwV3d &sundir )
{
    g_pDebug->printMsg( "Shadow Pass: begin", 1 );

    auto shadowRenderer = g_pDeferredRenderer->m_pShadowRenderer;
    m_uiDeferredStage = 2;
    // DefinedState has already forced rwCULLMODECULLNONE for the frame, and the cubemap pass sets it again, so
    // every cascade was rasterising both faces of every caster. Back-face culling is correct here because the
    // near surface seen from the sun is the one that casts. The mode goes back afterwards so this stays scoped
    // to the cascade pass and the G-buffer pass that follows keeps the state it had before.
    const RwCullMode savedCullMode = g_pStateMgr->GetCullMode();
    g_pStateMgr->SetCullMode( rwCULLMODECULLBACK );
    // with some cascades kept from earlier frames, each re-rendered cascade clears its own region instead
    if ( shadowRenderer->AllCascadesUpdated() )
        RwCameraClear( shadowRenderer->m_pShadowCamera, gColourTop, rwCAMERACLEARZ );

    // Render cascades
    for ( int i = 0; i < gShadowSettings.ShadowCascadeCount; i++ )
    {
        shadowRenderer->RenderShadowToBuffer( i, [] ( int k )
        {
            CRendererRH::RenderShadowCascade( k );
        } );
    }
    g_pStateMgr->SetCullMode( savedCullMode );
    shadowRenderer->m_bShadowsRendered = true;
    g_pDebug->printMsg( "Shadow Pass: end", 1 );
}

void CSAIdleHook::PrepareRenderStuff()
{
    RwV2d mousePos;
    mousePos.x = RsGlobal.maximumWidth * 0.5f;
    mousePos.y = RsGlobal.maximumHeight * 0.5f;
    POINT pMousePos{};
    // TODO: add custom key to show menu
    static bool s_bPrevGuiEnabled = false;
    static bool s_bPrevF12Down = false;
    bool bF12Down = ( GetKeyState( VK_F12 ) & 0x8000 ) != 0;
    if ( bF12Down && !s_bPrevF12Down )
    {
        if ( SettingsHolder::Instance()->IsGUIEnabled() )
            SettingsHolder::Instance()->DisableGUI();
        else
            SettingsHolder::Instance()->EnableGUI();
    }
    s_bPrevF12Down = bF12Down;
    bool guiEnabled = SettingsHolder::Instance()->IsGUIEnabled();
    if ( guiEnabled )
        SettingsHolder::Instance()->EnableGUI();
    else
        SettingsHolder::Instance()->DisableGUI();

    if ( guiEnabled != s_bPrevGuiEnabled )
    {
        ( (void( __cdecl * )())0x746F70 )();
        ( (void( __cdecl * )( int ))0x7469A0 )( guiEnabled ? 0 : 1 );
        if ( guiEnabled )
            RsMouseSetPos( &mousePos );
        s_bPrevGuiEnabled = guiEnabled;
    }
    HWND hWnd = guiEnabled ? static_cast<CRwD3D1XEngine*>( g_pRwCustomEngine )->getRenderer()->getHWND() : nullptr;
    const bool bHaveMousePos = guiEnabled && hWnd != nullptr && GetCursorPos( &pMousePos ) != FALSE &&
                               ScreenToClient( hWnd, &pMousePos ) != FALSE;
    if ( !guiEnabled )
    {
        // Let DirectInput handle in-game mouse movement exclusively without per-frame SetCursorPos interference
    }
    else if ( bHaveMousePos )
        TwMouseMotion( pMousePos.x, pMousePos.y );
    //ShowCursor(guiEnabled);
}

void CSAIdleHook::Render2dStuffAfterFade()
{
    CHud__DrawAfterFade();
    CMessages__Display( 0 ); // CMessages::Display
    CFont__RenderFontBuffer();     // CFont::DrawFonts
    if ( CCredits__bCreditsGoing )
    {
        if ( !FrontEndMenuManager.m_bMenuActive )
            CCredits__Render();       // CCredits::Render
    }
}

void CSAIdleHook::DoRWStuffEndOfFrame()
{
    DebugDisplayTextBuffer();
    FlushObrsPrintfs();
    RwCameraEndUpdate( Scene.m_pRwCamera );
    RsCameraShowRaster( Scene.m_pRwCamera );
}

void CSAIdleHook::PrepareRwCamera()
{
    CDraw::CalculateAspectRatio(); // CDraw::CalculateAspectRatio
    CameraSize( Scene.m_pRwCamera, 0, tanf( CDraw::ms_fFOV * ( 3.1415927f / 360.0f ) ), CDraw::ms_fAspectRatio );
    CVisibilityPlugins::SetRenderWareCamera( Scene.m_pRwCamera ); // CVisibilityPlugins::SetRenderWareCamera
    RwCameraClear( Scene.m_pRwCamera, gColourTop, rwCAMERACLEARZ );
}


void CSAIdleHook::LightUpdate()
{
    CLightManager::Reset();
    UpdateShadowDNBalance();
    SetLightsWithTimeOfDayColour( Scene.m_pRpWorld );
}

void CSAIdleHook::GameUpdate()
{
    // Reset before the process step, as the original idle does.
    CPointLights::NumLights = 0;
    CGame::Process();
    CAudioEngine__Service( AudioEngine );
}

void CSAIdleHook::InitPerFrame2D()
{
    CSprite2d::InitPerFrame();
    CFont::InitPerFrame();
}

void CSAIdleHook::TimeUpdate()
{
    // The original spins here until the millisecond clock leaves zero, which start up has already passed.
    CGame__LastTickTime = CTimer__GetTimeMillisecondsFromStart();
    CTimer__Update();
}

