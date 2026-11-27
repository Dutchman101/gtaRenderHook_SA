// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "StreamingRH.h"
#include <game_sa\CStreaming.h>

// CStreaming::Init2 reads both streamed rw object limits from its own instruction stream. The push at 0x5B8E54 sizes the
// allocation and the mov ecx at 0x5B8EAF feeds the init loop, whose operand is a byte extent, not a link count: each pass
// does sub ecx, 0Ch at 0x5B8EBA. Both hold 0x2EE0, 12000 bytes or a thousand links, and what they hold when the function
// reaches them sizes the list, so raising them afterwards repairs the bytes and not a list already built. The raise is
// repeated from the call to CExternalScripts::initialize, which Init2 reaches at +0x346 before the list exists.
constexpr UINT_PTR CEXTERNALSCRIPTS_INITIALIZE    = 0x470660;
constexpr UINT_PTR STREAMING_INIT2_PRE_LIST_CALL  = 0x5B8E16;

void CStreamingRH::RaiseStreamedObjectLimits( bool bReport )
{
    // The streamed rw object list. Each entry costs one link, so the limit is a link count and the byte count comes from
    // the link's own size rather than a number kept in step by hand. Only ever raised: AddEntity below deletes the rw
    // object of any entity it cant fit, so a lower limit is a world that keeps its collision and stops drawing.
    constexpr int STREAMED_OBJECT_LINKS      = 50000;
    constexpr int STREAMED_OBJECT_LIST_BYTES = static_cast<int>( sizeof( CLink<CEntity *> ) ) * STREAMED_OBJECT_LINKS;
    static_assert( sizeof( CLink<CEntity *> ) == 12,
                   "the limit is a byte count handed to an allocator, so a link whose size changed would resize the list "
                   "instead of just changing how many entries fit" );
    RaiseIntLimit( "Streaming: first rw object instance list size", 0x5B8E55, STREAMED_OBJECT_LIST_BYTES,
                   SITE_PREFIX_PUSH_IMM32, bReport );
    RaiseIntLimit( "Streaming: second rw object instance list size", 0x5B8EB0, STREAMED_OBJECT_LIST_BYTES,
                   SITE_PREFIX_MOV_ECX_IMM32, bReport );
}

// Reached from the replacement of the call above, on the game's init path. It raises both sites, then runs the call it
// displaced, so a module that had hooked that stub keeps the call it was relying on. The callee takes no stack argument
// and pops none: it reads its object from ecx, so a thiscall pointer reproduces exactly what the site did.
int __fastcall CStreamingRH::ReassertStreamedObjectLimits( int iThis, int iReserved )
{
    (void)iReserved;
    // Quiet, because this runs on the game's streaming path and a report from there waits on the logger's mutex across a
    // disk write, on a thread whose budget is measured in milliseconds.
    RaiseStreamedObjectLimits( false );
    return reinterpret_cast<int (__thiscall *)( int )>( CEXTERNALSCRIPTS_INITIALIZE )( iThis );
}

void CStreamingRH::Patch()
{
    // Raised here as well as from the hook, so a load after the game has already sized the list still gets the constants
    // raised for whatever reads them next. Registration is by site, so the hook's raise updates the row this one created;
    // the hook is what makes the raise reach the allocation itself.
    RaiseStreamedObjectLimits();
    InstallCall( STREAMING_INIT2_PRE_LIST_CALL,
                 reinterpret_cast<void *>( &CStreamingRH::ReassertStreamedObjectLimits ) );
    RedirectJump( 0x409650, AddEntity );
}

// Hope pluginsdk will include this method soon, starting to hate these definitions after having such convinient sdk
#define rw_objlink_Add(a,b) ((CLink<CEntity*> * (__thiscall*)(CLinkList<CEntity*> *, CEntity **))0x408230)(a,b)

CLink<CEntity*>* CStreamingRH::AddEntity( CEntity * pEntity )
{
    CLink<CEntity*> *result = nullptr;
    CLink<CEntity*> *currLink;

    // Raised here as well as from the hook over CStreaming::Init2, because a host that replaces or bypasses streaming init
    // never reaches that call, so whichever module wrote the two operands last would win. Sampled, because each raise asks
    // the system about two addresses and a busy streaming frame reaches this many times. Nothing is written unless lowered.
    constexpr unsigned long REASSERT_EVERY_CALLS = 64;
    static std::atomic<unsigned long> s_ulAddEntityCalls{ 0 };
    if ( ( s_ulAddEntityCalls.fetch_add( 1, std::memory_order_relaxed ) % REASSERT_EVERY_CALLS ) == 0 )
        RaiseStreamedObjectLimits( false );

    eEntityType entityType = (eEntityType)pEntity->m_nType;
    // Peds and vehicles are streamed differently, ignore them
    if ( entityType != eEntityType::ENTITY_TYPE_PED &&
         entityType != eEntityType::ENTITY_TYPE_VEHICLE )
    {
        result = rw_objlink_Add( &CStreaming::ms_rwObjectInstances, &pEntity );

        if ( result == nullptr ) // if we fail to add link than something has happend
        {
            // Held rather than reported, because a report from the streaming thread waits on the logger's mutex across a
            // file write; the frame path delivers it. Counted rather than raised once ever, because a note fired once per
            // process would leave a log unable to say whether it happened once or every frame.
            static std::atomic<unsigned long> s_ulListFullEvents{ 0 };
            constexpr unsigned long LIST_FULL_REPORT_INTERVAL = 4096;
            const unsigned long ulListFullEvents =
                s_ulListFullEvents.fetch_add( 1, std::memory_order_acq_rel ) + 1;
            if ( ulListFullEvents == 1 || ( ulListFullEvents % LIST_FULL_REPORT_INTERVAL ) == 0 )
            {
                char note[LATCHED_NOTE_TEXT_LENGTH];
                _snprintf_s( note, sizeof( note ), _TRUNCATE,
                             "is full for event %lu of this run, so this plugin's hook is deleting rw objects for "
                             "entities that are still in the world: those entities keep their collision and their "
                             "position and stop drawing. Reported on the first event and then once every %lu, and "
                             "whether this plugin could raise the list to the size it asks for is reported at load",
                             ulListFullEvents, LIST_FULL_REPORT_INTERVAL );
                LatchPatchNote( "Streaming: rw object instance list", note );
            }
            currLink = CStreaming::ms_rwObjectInstances.usedListTail.prev;

            // traverse the list from last link to head 
            while ( currLink != &CStreaming::ms_rwObjectInstances.usedListHead )
            {
                // if we found some entity that can be deleted - remove it's rwobject
                // The null test comes first because the link's entity is written by the game: reading it at a near null
                // address would happen only when the list is already full, which is the worst moment to lose.
                if ( currLink->data != nullptr && !currLink->data->m_bStreamingDontDelete &&
                     !currLink->data->m_bImBeingRendered )
                {
                    if ( currLink->data->m_pRwObject )
                        currLink->data->DeleteRwObject();
                    return nullptr;// rw_objlink_Add(&CStreaming::ms_rwObjectInstances, &pEntity);
                }
                currLink = currLink->prev;
            }
            // if list is full, than we remove rwobject of entity we try to add to it
            if ( pEntity->m_pRwObject )
                pEntity->DeleteRwObject();
            return nullptr;
        }
    }
    return result;
}
