// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "RwRenderEngine.h"
#include "RwD3D1XEngine.h"

bool CIRwRenderEngine::EventHandlingSystem( RwRenderSystemState nOption, int * pOut, void * pInOut, int nIn )
{
    std::string s = "Event handled: "; s += std::to_string( static_cast<int>( nOption ) );
    g_pDebug->printMsg( s.c_str(), 2 );
    switch ( nOption )
    {
    case RwRenderSystemState::rwDEVICESYSTEMOPEN:
        return Open( *static_cast<HWND*>( pInOut ) );
    case RwRenderSystemState::rwDEVICESYSTEMCLOSE:
        return Close();
    case RwRenderSystemState::rwDEVICESYSTEMSTART:
        return Start();
    case RwRenderSystemState::rwDEVICESYSTEMSTOP:
        return Stop();
    case RwRenderSystemState::rwDEVICESYSTEMGETNUMMODES:
        return GetNumModes( *pOut );
    case RwRenderSystemState::rwDEVICESYSTEMGETMODEINFO:
        return GetModeInfo( *(RwVideoMode*)( pOut ), nIn );
    case RwRenderSystemState::rwDEVICESYSTEMUSEMODE:
        return UseMode( nIn );
    case RwRenderSystemState::rwDEVICESYSTEMFOCUS:
        return Focus( nIn != 0 );
    case RwRenderSystemState::rwDEVICESYSTEMGETMODE:
        return GetMode( *pOut );
    case RwRenderSystemState::rwDEVICESYSTEMSTANDARDS:
        return Standards( (int*)pOut, nIn );
    case RwRenderSystemState::rwDEVICESYSTEMGETNUMSUBSYSTEMS:
        return GetNumSubSystems( *(int*)pOut );
    case RwRenderSystemState::rwDEVICESYSTEMGETSUBSYSTEMINFO:
        return GetSubSystemInfo( *(RwSubSystemInfo*)pOut, nIn );
    case RwRenderSystemState::rwDEVICESYSTEMGETCURRENTSUBSYSTEM:
        return GetCurrentSubSystem( *pOut );
    case RwRenderSystemState::rwDEVICESYSTEMSETSUBSYSTEM:
        return SetSubSystem( nIn );
    case RwRenderSystemState::rwDEVICESYSTEMGETTEXMEMSIZE:
        return GetTexMemSize( *pOut );
    case RwRenderSystemState::rwDEVICESYSTEMREGISTER:
    case RwRenderSystemState::rwDEVICESYSTEMINITPIPELINE:
    case RwRenderSystemState::rwDEVICESYSTEMFINALIZESTART:
    case RwRenderSystemState::rwDEVICESYSTEMINITIATESTOP:
    case RwRenderSystemState::rwDEVICESYSTEMRXPIPELINEREQUESTPIPE:
    case RwRenderSystemState::rwDEVICESYSTEMGETMETRICBLOCK:
        break;
    case RwRenderSystemState::rwDEVICESYSTEMGETMAXTEXTURESIZE:
        return GetMaxTextureSize( *pOut );
    case RwRenderSystemState::rwDEVICESYSTEMGETID:
        *pOut = 2;
        return true;
    default:
        break;
    }
    return BaseEventHandler( static_cast<int>( nOption ), pOut, pInOut, nIn );
}

bool mDefStd( void *, void *, int )
{
    return false;
}

bool mRasterCreate( void * a, void * b, int c )
{
    UNREFERENCED_PARAMETER( a );
    if ( !IsRendererDeviceReady() )
        return false;
    return g_pRwCustomEngine->RasterCreate( static_cast<RwRaster*>( b ), (UINT)c );
}

bool mNativeTextureRead( void * a, void * b, int c )
{
    UNREFERENCED_PARAMETER( c );
    if ( !IsRendererDeviceReady() )
    {
        // Cleared here as well as inside the engine, because this refusal sits above that function's own first line and
        // would otherwise leave the caller with whatever the slot held.
        if ( b != nullptr )
            *static_cast<RwTexture **>( b ) = nullptr;
        return false;
    }
    return g_pRwCustomEngine->NativeTextureRead( static_cast<RwStream*>( a ), static_cast<RwTexture**>( b ) );
}

bool mRasterLock( void * a, void * b, int c )
{
    if ( !IsRendererDeviceReady() )
    {
        // Required, not defensive: RasterLock clears this slot before testing anything so every one of its own refusals
        // leaves the caller with no buffer, and the texture reader writes lengthOut bytes through whatever pointer it is
        // handed. Skipping the clear here would hand that write the caller's old value.
        if ( a != nullptr )
            *static_cast<void **>( a ) = nullptr;
        return false;
    }
    return g_pRwCustomEngine->RasterLock( static_cast<RwRaster*>( b ), static_cast<UINT>( c ), static_cast<void**>( a ) );
}
bool mRasterUnlock( void * a, void * b, int c )
{
    UNREFERENCED_PARAMETER( a );
    UNREFERENCED_PARAMETER( c );
    if ( !IsRendererDeviceReady() )
        return false;
    return g_pRwCustomEngine->RasterUnlock( static_cast<RwRaster*>( b ) );
}
// These four are the frame itself, so they are refused until every manager and pipeline a drawn frame reads exists, not
// merely while a renderer does. Each returns the false it already returns for a call it couldnt complete.
bool mCamClear( void * a, void * b, int c )
{
    if ( !IsRenderFrameReady() )
        return false;
    return g_pRwCustomEngine->CameraClear( static_cast<RwCamera*>( a ), static_cast<RwRGBA*>( b ), c );
}

bool mCamBU( void * a, void * b, int c )
{
    UNREFERENCED_PARAMETER( a );
    UNREFERENCED_PARAMETER( c );
    if ( !IsRenderFrameReady() )
        return false;
    return g_pRwCustomEngine->CameraBeginUpdate( static_cast<RwCamera*>( b ) );
}

bool mCameraEndUpdate( void * a, void * b, int c )
{
    UNREFERENCED_PARAMETER( a );
    UNREFERENCED_PARAMETER( c );
    if ( !IsRenderFrameReady() )
        return false;
    return g_pRwCustomEngine->CameraEndUpdate( static_cast<RwCamera*>( b ) );
}
bool mRasterShowRaster( void * a, void * b, int c )
{
    UNREFERENCED_PARAMETER( b );
    if ( !IsRenderFrameReady() )
        return false;
    return g_pRwCustomEngine->RasterShowRaster( static_cast<RwRaster*>( a ), (UINT)c );
}
// The release this ends in deletes the texture object, whose destructor releases the Direct3D children the device owns, so
// this is gated on the same test as the entry points above. The context half is wider than needed and kept, because only
// DeInitDevice separates the two. A refused destroy leaves the raster where one arriving after a Stop already leaves it.
bool mRasterDestroy( void * a, void * b, int c )
{
    UNREFERENCED_PARAMETER( a );
    UNREFERENCED_PARAMETER( c );
    if ( !IsRendererDeviceReady() )
        return false;
    return g_pRwCustomEngine->RasterDestroy( static_cast<RwRaster*>( b ) );
}