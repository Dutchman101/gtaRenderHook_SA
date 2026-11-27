#pragma once
#include <game_sa\CEntity.h>
#include <game_sa\CLinkList.h>
class CStreamingRH
{
public:
    static void Patch();
    /*
        Fixed function for original GTA SA memory leak when there is more than 1000 entities
        streamed simultaneously.
    */
    static CLink<CEntity*>* AddEntity( CEntity *pEntity );
    // Raises both streamed rw object list limits to the byte count we size the list for. Called from the patch batch at load,
    // again from the hook over CStreaming::Init2, and again from AddEntity every sixty fourth call so a host that never
    // reaches Init2 still re-raises them. Every caller but the load time one runs on the streaming path and passes false.
    static void RaiseStreamedObjectLimits( bool bReport = true );
    // Runs in place of the call to CExternalScripts::initialize inside CStreaming::Init2, which is after every module has
    // written and before the list is sized. Its argument list is the displaced stub's, forwarded as is.
    static int __fastcall ReassertStreamedObjectLimits( int iThis, int iReserved );
};

