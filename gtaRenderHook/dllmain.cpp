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
    char report[256];
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
    g_pDebug->printRecord( report );
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
)
{
    UNREFERENCED_PARAMETER( lpReserved );
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

        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        break;
    case DLL_PROCESS_DETACH:
        // The engine holds the debug sink it was constructed with, so it has to be destroyed
        // while that sink is still alive.
        delete g_pRwCustomEngine;
        g_pRwCustomEngine = nullptr;
        delete g_pDebug;
        g_pDebug = nullptr;
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