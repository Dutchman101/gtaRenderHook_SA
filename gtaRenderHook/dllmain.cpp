// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "CDebug.h"
//#include "RwRenderEngine.h"
#include "RwD3D1XEngine.h"
#include "SAIdleHook.h"
#include "CustomBuildingPipeline.h"
#include "CustomBuildingDNPipeline.h"
#include "CustomCarFXPipeline.h"
#include "CustomSeabedPipeline.h"
#include "CustomWaterPipeline.h"
#include "DeferredRenderer.h"
#include "VoxelOctreeRenderer.h"
#include "LightManager.h"
#include "FullscreenQuad.h"
#include "D3D1XTextureMemoryManager.h"
#include "DebugBBox.h"
#include "HDRTonemapping.h"
#include "ShadowRenderer.h"
#include "gta_sa_ptrs.h"
#include "SettingsHolder.h"
#include "D3DRenderer.h"
#include "PBSMaterial.h"
#include "D3D1XStateManager.h"
#include "CVisibilityPluginsRH.h"
#include <game_sa\CModelInfo.h>
#include <game_sa\CVehicle.h>
#include <game_sa\CRadar.h>
#include <game_sa\CGame.h>
#include "DebugRendering.h"
#include "VolumetricLighting.h"
#include "AmbientOcclusion.h"
#include <game_sa\CStreaming.h>
#include <game_sa\CEntity.h>
#include "CRwGameHooks.h"
#include "GTASAHooks.h"
#include "StreamingRH.h"
#include "SampHaxx.h"
#include "TemporalAA.h"
CDebug*				g_pDebug;
CIRwRenderEngine*	g_pRwCustomEngine;

// The writers in MemoryFuncs.h report through here, so that header needs no logger of its own to link against.
static void ReportPatchFailureToLog( const char *operation, void *address, const char *reason )
{
    if ( !g_pDebug || !operation || !reason )
        return;
    // Sized for the longest line the writers can send: a restore naming the site, the reason, and the state the site was
    // left in. The note sink below is the same size.
    char report[512];
    _snprintf_s( report, sizeof( report ), _TRUNCATE, "%s: the site at 0x%08X %s", operation,
                 static_cast<unsigned int>( reinterpret_cast<UINT_PTR>( address ) ), reason );
    g_pDebug->printError( report );
}

// The other half of the same sink, written as a record because a finished write isnt a fault. Goes through the
// ungated channel, since DebugLevel 0 silences printMsg.
static void ReportPatchSuccessToLog( const char *operation, void *address )
{
    if ( !g_pDebug || !operation )
        return;
    char report[256];
    _snprintf_s( report, sizeof( report ), _TRUNCATE,
                 "%s: the site at 0x%08X was written, the processor was told those bytes changed, and the page has "
                 "its own protection back",
                 operation, static_cast<unsigned int>( reinterpret_cast<UINT_PTR>( address ) ) );
    // Discarded on purpose, and this is the sink that may. A finished write is reported once per install and no caller
    // spends a claim on that line, so a lost one leaves nothing a retry would recover: the site's own row and the frame
    // check are what answer whether the write still stands. The note sink keeps its answer because the audit spends a claim.
    (void)g_pDebug->printRecord( report );
}

// Neither a refusal nor a finished write: a limit another tool already raised, a site another module had written before we
// reached it, or a row of the site audit. Answers whether the record reached the file: the site audit is the one caller
// that cant ignore a no, because a false is what lets it spend its once-a-run claim again later.
static bool ReportPatchNoteToLog( const char *operation, const char *note )
{
    if ( !g_pDebug || !operation || !note )
        return false;
    char report[512];
    _snprintf_s( report, sizeof( report ), _TRUNCATE, "%s %s", operation, note );
    return g_pDebug->printRecord( report );
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
)
{
    UNREFERENCED_PARAMETER( hModule );
    switch ( ul_reason_for_call )
    {
    case DLL_PROCESS_ATTACH:
        // Load RenderHook settings
        SettingsHolder::Instance()->ReloadFile();
        // Init basic stuff
        g_pDebug = new CDebug( "debug.log" );
        // Before the first patch, so no refusal is dropped for want of a logger.
        SetPatchFailureSink( ReportPatchFailureToLog );
        SetPatchSuccessSink( ReportPatchSuccessToLog );
        SetPatchNoteSink( ReportPatchNoteToLog );
        g_pRwCustomEngine = new CRwD3D1XEngine( g_pDebug );

        // Path rw/game stuff
        CRwGameHooks::Patch( CRwGameHooks::ms_rwPointerTableSA );
        CGTASAHooks::Patch();
        // Replace all pipelines(move to CRwGameHooks or CGTASAHooks)
        CCustomBuildingPipeline::Patch();
        CCustomBuildingDNPipeline::Patch();
        CCustomCarFXPipeline::Patch();
        SampHaxx::Patch();

        // One line for the whole batch, since a refused write leaves the plugin running with part of its hook set.
        // Braced because a case label would otherwise jump past this initialization.
        {
            const unsigned long ulRefusals = PatchRefusalCount();
            if ( ulRefusals != 0 )
            {
                char report[256];
                // Verb and auxiliary follow the count, so a single refusal reads correctly.
                _snprintf_s( report, sizeof( report ), _TRUNCATE,
                             "Patch batch: %lu of this plugin's patch writes %s refused and %s named above, so this "
                             "plugin is running with the hooks that were written and without the rest",
                             ulRefusals, ulRefusals == 1 ? "was" : "were", ulRefusals == 1 ? "is" : "are" );
                g_pDebug->printError( report );
            }
        }

        // One line naming the behaviours this build carries, so a log says which build produced it. A fixed literal that
        // changes only when a behaviour changes, with no build time and no version stamp, because a build that embedded one
        // would depend on when it was built rather than on what it was built from.
        (void)g_pDebug->printRecord( "RenderHook behaviours: value and code sites owned and checked every frame, both "
                                     "streamed rw object list limits raised from the frame path and from the streaming "
                                     "hook, D3D device creation staged before assignment, adapter paired only with the "
                                     "unknown driver type, frame entry points refused while no frame can be drawn, log "
                                     "dedupe shared across threads, and a read only site watch written beside the log" );

        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        // lpReserved is non-null when the process is terminating, which is the documented signal that teardown is not
        // required: the teardown below releases COM objects and shuts the MTA bridge down, and by now the loader has begun
        // unloading other modules, so running it here tears down an interop link whose own modules may have left. The site
        // watch is asked to stop on both paths, because a pass beginning after termination started would write into a
        // process that is going away; only an ordinary unload waits for it and closes the handles.
        RequestStopSiteWatchThread();
        if ( lpReserved == nullptr )
        {
            // Stopped before anything the thread could read is released. It reads only the two site tables, which are
            // function local statics living for the whole process, so nothing else depends on this order.
            StopSiteWatchThread();
            // The engine holds the debug sink it was constructed with, so it has to be destroyed
            // while that sink is still alive.
            delete g_pRwCustomEngine;
            g_pRwCustomEngine = nullptr;
            delete g_pDebug;
            g_pDebug = nullptr;
        }
        break;
    }
    return TRUE;
}

// TODO: move this out of here
RxD3D9InstanceData* GetModelsData( RxInstanceData * data )
{
    return reinterpret_cast<RxD3D9InstanceData*>( data + 1 );
}

RxD3D9InstanceData *GetModelsData2( RxD3D9ResEntryHeader *data )
{
    return reinterpret_cast<RxD3D9InstanceData *>( data + 1 );
}

RpMesh * GetMeshesData( RpD3DMeshHeader * data )
{
    return reinterpret_cast<RpMesh*>( data + 1 );
}