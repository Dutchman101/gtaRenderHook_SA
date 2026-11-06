// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "RwD3D1XEngine.h"
#include "D3D1XStateManager.h"
#include "D3D1X2DTexture.h"
#include "D3D1X3DTexture.h"
#include "D3D1X2DRenderTarget.h"
#include "D3D1XBackBufferTexture.h"
#include "D3D1XDepthStencilTexture.h"
#include "D3D1XDefaultPipeline.h"
#include "D3D1XSkinPipeline.h"
#include "D3D1XEnumParser.h"
#include "D3DRenderer.h"
#include "D3D1XIm2DPipeline.h"
#include "D3D1XIm3DPipeline.h"
#include "CustomBuildingPipeline.h"
#include "CustomBuildingDNPipeline.h"
#include "CustomCarFXPipeline.h"
#include "DeferredRenderer.h"
#include "D3D1XVertexDeclarationManager.h"
#include <game_sa\CScene.h>
#include "D3D1XRenderBuffersManager.h"
#include "D3D1XTextureMemoryManager.h"
#include "CustomSeabedPipeline.h"
#include "CustomWaterPipeline.h" 
#include "D3D1XVertexBufferManager.h"
#include "D3D1XIndexBuffer.h"
#include "D3D1XShaderDefines.h" 

#ifdef USE_ANTTWEAKBAR
#include <AntTweakBar.h>
#endif
extern CD3D1XStateManager* g_pStateMgr = nullptr;
extern CD3D1XRenderBuffersManager* g_pRenderBuffersMgr = nullptr;
extern CCustomBuildingPipeline* g_pCustomBuildingPipe = nullptr;
extern CCustomBuildingDNPipeline* g_pCustomBuildingDNPipe = nullptr;
extern CCustomCarFXPipeline* g_pCustomCarFXPipe = nullptr;
extern CDeferredRenderer* g_pDeferredRenderer = nullptr;
extern CCustomSeabedPipeline* g_pCustomSeabedPipe = nullptr;
extern CCustomWaterPipeline* g_pCustomWaterPipe = nullptr;
ShaderRenderStateBuffer g_shaderRenderStateBuffer;

CRwD3D1XEngine::CRwD3D1XEngine( CDebug *d ) : CIRwRenderEngine{ d }
{
}

// MAIN FUNCTIONS, handle initialization and deinitialization

bool CRwD3D1XEngine::Open( HWND window )
{
    m_pRenderer = new CD3DRenderer( window );
    return true;
}

bool CRwD3D1XEngine::Close()
{
    // Open assigns this pointer and Close is its matching teardown, so it is cleared here: a second
    // Close, or a Stop that arrives after it, would otherwise reach an object that no longer exists.
    delete m_pRenderer;
    m_pRenderer = nullptr;
    return true;
}

// Initializes device, all managers and rendering pipelines.
bool CRwD3D1XEngine::Start()
{
    // Every pipeline and manager below creates Direct3D 11 objects, so a device that failed to come
    // up has to stop the startup here instead of letting each constructor run against a null device.
    // The report goes through printError rather than printMsg because DebugMessaging defaults to off,
    // and a startup that stops has to leave a record even then.
    // The renderer is opened by the game and closed before the next open, so it can be absent here.
    if ( m_pRenderer == nullptr )
    {
        g_pDebug->printError( "RenderHook startup aborted: no renderer is open, so the device was not initialized." );
        return false;
    }
    if ( !m_pRenderer->InitDevice() )
    {
        // InitDevice names the reason it returned false, and one of those reasons is an adapter or
        // display mode list that enumerated nothing, where no device creation was attempted at all.
        // This line therefore reports only what it can establish here: startup stopped before the
        // rendering pipelines were created.
        g_pDebug->printError( "RenderHook startup aborted: the device was not initialized, so the render pipelines were not created." );
        return false;
    }
    g_pGlobalShaderDefines = new CD3D1XShaderDefineList();
    const auto featureLevel = GET_D3D_FEATURE_LVL;
    auto featureLvl = to_string( featureLevel );
    g_pGlobalShaderDefines->AddDefine( "FEATURE_LEVEL", featureLvl );
    g_pGlobalShaderDefines->AddDefine( "USE_PBR", to_string( (int)gShaderDefineSettings.GetToggleField( "UsePBR" ) ) );
    g_pStateMgr = new CD3D1XStateManager();
    g_pRenderBuffersMgr = new CD3D1XRenderBuffersManager();
    m_pIm2DPipe = new CD3D1XIm2DPipeline();
    m_pIm3DPipe = new CD3D1XIm3DPipeline();
    m_pDefaultPipe = new CD3D1XDefaultPipeline();
    m_pSkinPipe = new CD3D1XSkinPipeline();
#ifdef USE_ANTTWEAKBAR
    if ( !TwInit( TwGraphAPI::TW_DIRECT3D11, m_pRenderer->getDevice() ) )
        g_pDebug->printMsg( "Failed to initialize AntTweakBar, in-game settings will be disabled.", 0 );
    else
    {
        TwWindowSize( m_pRenderer->getCurrentAdapterModeDesc().Width, m_pRenderer->getCurrentAdapterModeDesc().Height );
    }
#endif
    g_pStateMgr->SetScreenSize( static_cast<float>( m_pRenderer->getCurrentAdapterModeDesc().Width ), static_cast<float>( m_pRenderer->getCurrentAdapterModeDesc().Height ) );
    return true;
}

// Deallocates pipelines and managers.
bool CRwD3D1XEngine::Stop()
{
#ifdef USE_ANTTWEAKBAR
    TwTerminate();
#endif
    // Each pointer is cleared immediately after it is released. Start() can now return early without
    // reassigning them, so a released object left reachable here would be freed a second time by the
    // next Stop(), and a repeated Stop() has to stay safe.
    delete m_pSkinPipe;
    m_pSkinPipe = nullptr;
    delete m_pDefaultPipe;
    m_pDefaultPipe = nullptr;
    delete m_pIm2DPipe;
    m_pIm2DPipe = nullptr;
    delete m_pIm3DPipe;
    m_pIm3DPipe = nullptr;
    delete g_pRenderBuffersMgr;
    g_pRenderBuffersMgr = nullptr;
    delete g_pStateMgr;
    g_pStateMgr = nullptr;
    // The define list is created by Start(), so it is released here as well or every Start/Stop cycle
    // leaks it. It goes after the pipelines because shader compiles read it while they exist.
    delete g_pGlobalShaderDefines;
    g_pGlobalShaderDefines = nullptr;
    CD3D1XVertexBufferManager::Shutdown();
    CD3D1XVertexDeclarationManager::Shutdown();
    CD3D1XTextureMemoryManager::Shutdown();
    // Close can run before Stop, so the renderer may already be absent.
    if ( m_pRenderer )
        m_pRenderer->DeInitDevice();
    return true;
}

// VIDEOMODE FUNCTIONS, handle video adapter modes(screen size, refresh rate etc.)

// Returns video adapter mode count
bool CRwD3D1XEngine::GetNumModes( int& n )
{
    n = m_pRenderer->getAdapterModeCount();
    return true;
}

// Returns video mode information
bool CRwD3D1XEngine::GetModeInfo( RwVideoMode& mode, int n )
{
    DXGI_MODE_DESC desc = m_pRenderer->getAdapterModeDesc( n );
    mode = { static_cast<RwInt32>( desc.Width ), static_cast<RwInt32>( desc.Height ), 32, rwVIDEOMODEEXCLUSIVE, static_cast<RwInt32>( desc.RefreshRate.Numerator ), rwRASTERFORMATDEFAULT };
    return true;
}

// Selects current adapter mode.
bool CRwD3D1XEngine::UseMode( int n )
{
    m_pRenderer->setCurrentAdapterMode( n );
    return true;
}

// Returns current adapter mode.
bool CRwD3D1XEngine::GetMode( int& n )
{
    n = m_pRenderer->getCurrentAdapterMode();
    return true;
}


bool CRwD3D1XEngine::Focus( bool )
{
    g_pDebug->printError( "RW Focus is not implemented; the call is accepted." );
    return true;
}

bool CRwD3D1XEngine::Standards( int* fnPtrArray, int )
{
    bool( *pDefstd )( void *, void *, int ) = &mDefStd;
    bool( *pRasterCreate )( void *, void *, int ) = &mRasterCreate;
    bool( *pNativeTextureRead )( void *, void *, int ) = &mNativeTextureRead;
    bool( *pRasterLock )( void *, void *, int ) = &mRasterLock;
    bool( *pRasterUnlock )( void *, void *, int ) = &mRasterUnlock;
    bool( *pClear )( void *, void *, int ) = &mCamClear;
    bool( *pBeginUpdate )( void *, void *, int ) = &mCamBU;
    bool( *pEndUpdate )( void *, void *, int ) = &mCameraEndUpdate;
    bool( *pRasterShowRaster )( void *, void *, int ) = &mRasterShowRaster;
    bool( *pRasterDestroy )( void *, void *, int ) = &mRasterDestroy;

    fnPtrArray[0] = (int)(void*&)pDefstd;
    fnPtrArray[1] = (int)(void*&)pBeginUpdate;//0x7F8F20;//CameraBeginUpdate !!must do!!
    fnPtrArray[2] = 0x7FEE20;//RGBToPixel not used in menu
    fnPtrArray[3] = 0x7FF070;//PixelToRGB not used in menu
    fnPtrArray[4] = (int)(void*&)pRasterCreate;//0x4CCE60;//RasterCreate !!must do!!
    fnPtrArray[5] = (int)(void*&)pRasterDestroy;//0x4CBB00;//RasterDestroy !!must do!!
    fnPtrArray[6] = 0x7FF270;//ImageGetFromRaster not used in menu
    fnPtrArray[7] = (int)(void*&)pDefstd;//0x8001E0;//RasterSetFromImage not used in menu
    fnPtrArray[8] = 0x4CBD40;//TextureSetRaster not used in menu
    fnPtrArray[9] = (int)(void*&)pDefstd;//0x7FFF00;//ImageFindRasterFormat not used in menu
    fnPtrArray[10] = (int)(void*&)pEndUpdate;//0x7F98D0;//CameraEndUpdate !!must do!!
    fnPtrArray[11] = 0x4CB524;//SetRasterContext not used in menu
    fnPtrArray[12] = 0x4CBD50;//RasterSubRaster not used in menu
    fnPtrArray[13] = 0x4CB4C0;//RasterClearRect not used in menu
    fnPtrArray[14] = 0x4CB4E0;//RasterClear not used in menu
    fnPtrArray[15] = (int)(void*&)pRasterLock;//0x4C9F90;//RasterLock !!must do!!
    fnPtrArray[16] = (int)(void*&)pRasterUnlock;//0x4CA290;//RasterUnlock !!must do!!
    fnPtrArray[17] = 0x4CAE40;//RasterRender not used in menu
    fnPtrArray[18] = 0x4CAE80;//RasterRenderScaled not used in menu
    fnPtrArray[19] = 0x4CAE60;//RasterRenderFast not used in menu
    fnPtrArray[20] = (int)(void*&)pRasterShowRaster;//0x7F99B0;//RasterShowRaster !!must do!!
    fnPtrArray[21] = (int)(void*&)pClear;//0x4CB4E0;//CameraClear !!must do!!
    fnPtrArray[22] = (int)(void*&)pDefstd;
    fnPtrArray[23] = 0x4CA4E0;//RasterLockPalette not used in menu
    fnPtrArray[24] = 0x4CA540;//RasterUnlockPalette not used in menu
    fnPtrArray[25] = 0x4CD360;//NativeTextureGetSize not used in menu
    fnPtrArray[26] = (int)(void*&)pNativeTextureRead;//0x4CD820;//NativeTextureRead !!must do!!
    fnPtrArray[27] = 0x4CD4D0;//NativeTextureWrite not used in menu
    fnPtrArray[28] = 0x4CBCB0;//RasterGetMipLevels not used in menu
    return true;
}

// SUB-SYSTEM aka VIDEOCARD FUNCTIONS, handle currently used video adapter

bool CRwD3D1XEngine::GetNumSubSystems( int& n )
{
    n = m_pRenderer->getAdapterCount();
    return true;
}

bool CRwD3D1XEngine::GetSubSystemInfo( RwSubSystemInfo& info, int n )
{
    strncpy_s( info.name, m_pRenderer->getAdapterInfo( n ), 80 );
    return true;
}

bool CRwD3D1XEngine::GetCurrentSubSystem( int& n )
{
    n = m_pRenderer->getCurrentAdapter();
    return true;
}

bool CRwD3D1XEngine::SetSubSystem( int n )
{
    m_pRenderer->setCurrentAdapter( n );
    return true;
}

// UNUSED FUNCTIONS, for multisampling and stuff(TODO: make them great again!)

bool CRwD3D1XEngine::GetTexMemSize( int& memSize )
{
    memSize = m_pRenderer->getAvaliableTextureMemory();
    return true;
}

bool CRwD3D1XEngine::GetMaxTextureSize( int& texSize )
{
    auto featureLevel = m_pRenderer->getFeatureLevel();
    // DX11 has concrete maximum texture dimensions that depends on hardware feature level
    switch ( featureLevel )
    {
    case D3D_FEATURE_LEVEL_9_1:
    case D3D_FEATURE_LEVEL_9_2:
        texSize = 2048;
        break;
    case D3D_FEATURE_LEVEL_9_3:
        texSize = 4096;
        break;
    case D3D_FEATURE_LEVEL_10_0:
    case D3D_FEATURE_LEVEL_10_1:
        texSize = 8192;
        break;
    case D3D_FEATURE_LEVEL_11_0:
    case D3D_FEATURE_LEVEL_11_1:
    case D3D_FEATURE_LEVEL_12_0:
    case D3D_FEATURE_LEVEL_12_1:
        texSize = 16384;
        break;
    default:
        texSize = 0;
        break;
    }

    return true;
}

int  CRwD3D1XEngine::GetMaxMultiSamplingLevels()
{
    // currently no multisampling, will be added later 
    g_pDebug->printError( "RW GetMaxMultiSamplingLevels is not implemented; returning 0." );
    return 0;
}

void CRwD3D1XEngine::SetMultiSamplingLevels( int )
{
    // currently no multisampling, will be added later
    g_pDebug->printError( "RW SetMultiSamplingLevels is not implemented; the call is ignored." );
}

bool CRwD3D1XEngine::BaseEventHandler( int State, int* a2, void* a3, int a4 )
{
    return RwD3DSystem( State, a2, a3, a4 );
}

// RENDERSTATE FUNCTIONS

bool CRwD3D1XEngine::RenderStateSet( RwRenderState rs, UINT data )
{
    switch ( rs )
    {
    case rwRENDERSTATETEXTURERASTER:	// Sets texture raster renderstate, try to avoid using it 
        g_pStateMgr->SetRaster( reinterpret_cast<RwRaster*>( data ) );
        break;
    case rwRENDERSTATETEXTUREADDRESS:
        g_pStateMgr->SetTextureAdressUV( static_cast<RwTextureAddressMode>( (int)data ) );
        break;
    case rwRENDERSTATETEXTUREADDRESSU:
        g_pStateMgr->SetTextureAdressU( static_cast<RwTextureAddressMode>( (int)data ) );
        break;
    case rwRENDERSTATETEXTUREADDRESSV:
        g_pStateMgr->SetTextureAdressV( static_cast<RwTextureAddressMode>( (int)data ) );
        break;
    case rwRENDERSTATETEXTUREPERSPECTIVE:
        break;
    case rwRENDERSTATEZTESTENABLE:
        g_pStateMgr->SetDepthEnable( data != 0 );
        break;
    case rwRENDERSTATESHADEMODE:
        break;
    case rwRENDERSTATEZWRITEENABLE:
        g_pStateMgr->SetZWriteEnable( data != 0 );
        break;
    case rwRENDERSTATETEXTUREFILTER:
        g_pStateMgr->SetTextureFilterMode( static_cast<RwTextureFilterMode>( (int)data ) );
        break;
    case rwRENDERSTATESRCBLEND:
        g_pStateMgr->SetSrcAlphaBlend( static_cast<RwBlendFunction>( (int)data ) );
        break;
    case rwRENDERSTATEDESTBLEND:
        g_pStateMgr->SetDestAlphaBlend( static_cast<RwBlendFunction>( (int)data ) );
        break;
    case rwRENDERSTATEVERTEXALPHAENABLE:
        g_pStateMgr->SetAlphaBlendEnable( data != 0 );
        break;
    case rwRENDERSTATEBORDERCOLOR:
        g_pStateMgr->SetTextureBorderColor( ( *(RwRGBA*)&data ) );
        break;
    case rwRENDERSTATEFOGENABLE:
        break;
    case rwRENDERSTATEFOGCOLOR:
        break;
    case rwRENDERSTATEFOGTYPE:
        break;
    case rwRENDERSTATEFOGDENSITY:
        break;
    case rwRENDERSTATECULLMODE:
        g_pStateMgr->SetCullMode( static_cast<RwCullMode>( data ) );
        break;
    case rwRENDERSTATESTENCILENABLE:
        g_pStateMgr->SetStencilEnable( data != 0 );
        break;
    case rwRENDERSTATESTENCILFAIL:
        g_pStateMgr->SetStencilFail( static_cast<RwStencilOperation>( data ) );
        break;
    case rwRENDERSTATESTENCILZFAIL:
        g_pStateMgr->SetStencilZFail( static_cast<RwStencilOperation>( data ) );
        break;
    case rwRENDERSTATESTENCILPASS:
        g_pStateMgr->SetStencilPass( static_cast<RwStencilOperation>( data ) );
        break;
    case rwRENDERSTATESTENCILFUNCTION:
        g_pStateMgr->SetStencilFunc( static_cast<RwStencilFunction>( data ) );
        break;
    case rwRENDERSTATESTENCILFUNCTIONREF:
        g_pStateMgr->SetStencilFuncRef( (int)data );
        break;
    case rwRENDERSTATESTENCILFUNCTIONMASK:
        g_pStateMgr->SetStencilFuncMask( (int)data );
        break;
    case rwRENDERSTATESTENCILFUNCTIONWRITEMASK:
        g_pStateMgr->SetStencilFuncWriteMask( (int)data );
        break;
    case rwRENDERSTATEALPHATESTFUNCTION:
        g_pStateMgr->SetAlphaTestFunc( (RwAlphaTestFunction)data );
        break;
    case rwRENDERSTATEALPHATESTFUNCTIONREF:
        g_pStateMgr->SetAlphaTestRef( data / 255.0f );
        break;
    default:
        break;
    }
    return true;
}

bool CRwD3D1XEngine::RenderStateGet( RwRenderState rs, UINT& data )
{
    data = 0;
    switch ( rs )
    {
    case rwRENDERSTATETEXTURERASTER:
        data = (UINT)g_pStateMgr->GetRaster();
        break;
    case rwRENDERSTATETEXTUREADDRESS:
        data = (UINT)g_pStateMgr->GetTextureAdressUV();
        break;
    case rwRENDERSTATETEXTUREADDRESSU:
        data = (UINT)g_pStateMgr->GetTextureAdressU();
        break;
    case rwRENDERSTATETEXTUREADDRESSV:
        data = (UINT)g_pStateMgr->GetTextureAdressV();
        break;
    case rwRENDERSTATETEXTUREPERSPECTIVE:
        break;
    case rwRENDERSTATEZTESTENABLE:
        data = g_pStateMgr->GetDepthEnable();
        break;
    case rwRENDERSTATESHADEMODE:
        break;
    case rwRENDERSTATEZWRITEENABLE:
        data = g_pStateMgr->GetZWriteEnable();
        break;
    case rwRENDERSTATETEXTUREFILTER:
        data = (UINT)g_pStateMgr->GetTextureFilterMode();
        break;
    case rwRENDERSTATESRCBLEND:
        data = (UINT)g_pStateMgr->GetSrcAlphaBlend();
        break;
    case rwRENDERSTATEDESTBLEND:
        data = (UINT)g_pStateMgr->GetDestAlphaBlend();
        break;
    case rwRENDERSTATEVERTEXALPHAENABLE:
        data = (UINT)g_pStateMgr->GetAlphaBlendEnable();
        break;
    case rwRENDERSTATEBORDERCOLOR:
        break;
    case rwRENDERSTATEFOGENABLE:
        break;
    case rwRENDERSTATEFOGCOLOR:
        break;
    case rwRENDERSTATEFOGTYPE:
        break;
    case rwRENDERSTATEFOGDENSITY:
        break;
    case rwRENDERSTATECULLMODE:
        data = (UINT)g_pStateMgr->GetCullMode();
        break;
    case rwRENDERSTATESTENCILENABLE:
        break;
    case rwRENDERSTATESTENCILFAIL:
        break;
    case rwRENDERSTATESTENCILZFAIL:
        break;
    case rwRENDERSTATESTENCILPASS:
        break;
    case rwRENDERSTATESTENCILFUNCTION:
        break;
    case rwRENDERSTATESTENCILFUNCTIONREF:
        break;
    case rwRENDERSTATESTENCILFUNCTIONMASK:
        break;
    case rwRENDERSTATESTENCILFUNCTIONWRITEMASK:
        break;
    case rwRENDERSTATEALPHATESTFUNCTION:
        break;
    case rwRENDERSTATEALPHATESTFUNCTIONREF:
        data = g_pStateMgr->GetAlphaTestRef();
        break;
    default:
        break;
    }
    return true;
}

// IM2D PIPELINE FUNCTIONS

bool CRwD3D1XEngine::Im2DRenderPrimitive( RwPrimitiveType primType, RwIm2DVertex *vertices, RwUInt32 numVertices )
{
    m_pIm2DPipe->Draw( primType, vertices, numVertices );
    return true;
}

bool CRwD3D1XEngine::Im2DRenderIndexedPrimitive( RwPrimitiveType primType, RwIm2DVertex *vertices, RwUInt32 numVertices, RwImVertexIndex *indices, RwInt32 numIndices )
{
    // The count is signed here and unsigned in the pipeline, so a negative count would become a
    // buffer size request of several billion indices.
    if ( numIndices < 0 )
        return false;
    m_pIm2DPipe->DrawIndexed( primType, vertices, numVertices, indices, numIndices );
    return true;
}

// RASTER FUNCTIONS

bool CRwD3D1XEngine::RasterCreate( RwRaster *raster, UINT flags )
{
    RwD3D1XRaster* d3dRaster = GetD3D1XRaster( raster );
    // Init renderware raster data
    raster->cType = flags & rwRASTERTYPEMASK;
    raster->cFlags = flags & (RwUInt8)~rwRASTERTYPEMASK;
    raster->cpPixels = nullptr;
    raster->palette = nullptr;

    // Init directx raster data
    d3dRaster->resourse = nullptr;
    d3dRaster->palette = nullptr;
    d3dRaster->alpha = 0;
    d3dRaster->textureFlags = 0;
    d3dRaster->cubeTextureFlags = 0;
    d3dRaster->lockFlags = 0;
    d3dRaster->lockedSurface = 0;
    d3dRaster->format = DXGI_FORMAT_UNKNOWN;

    RwUInt32 rasterPixelFmt = flags & rwRASTERFORMATPIXELFORMATMASK;
    raster->cFormat = static_cast<RwUInt8>( rasterPixelFmt >> 8 );

    int maxTextureSize;
    GetMaxTextureSize( maxTextureSize );

    // If raster size exceeds maximum for current feature level we shouldn't create it.
    if ( raster->width > maxTextureSize || raster->height > maxTextureSize )
        return false;
    // Some rasters don't need to be allocated, skip these
    //if (flags & rwRASTERDONTALLOCATE)
    //	return true;
    CD3D1XEnumParser::ConvertRasterFormat( raster, flags );
    // If somehow after format conversion, raster format is still unknown, and it isn't camera raster we shouldn't create it.
    if ( d3dRaster->format == DXGI_FORMAT_UNKNOWN && raster->cType != rwRASTERTYPECAMERA )
        return false;
    std::string str = "2DTexture";
    constexpr auto tex3d_type = ( rwRASTERTYPECAMERATEXTURE + 1 );
    switch ( raster->cType )
    {
    case rwRASTERTYPENORMAL:
    case rwRASTERTYPETEXTURE:
        d3dRaster->resourse = new CD3D1X2DTexture( raster, (D3D11_BIND_FLAG)0, str, ( flags&rwRASTERFORMATMIPMAP ) != 0, true );
        break;
    case rwRASTERTYPEZBUFFER:
        d3dRaster->resourse = new CD3D1XDepthStencilTexture( raster );
        break;
    case rwRASTERTYPECAMERA:
        d3dRaster->resourse = new CD3D1XBackBufferTexture( raster );
        break;
    case rwRASTERTYPECAMERATEXTURE:
        d3dRaster->resourse = new CD3D1X2DRenderTarget( raster );
        break;
    case tex3d_type:
        d3dRaster->resourse = nullptr; // new CD3D1X3DTexture( raster );
        break;
    default:
        break;
    }
    // If we succes in allocating texture we should add it to texture memory manager to avoid some unresonable memory leaks.
    if ( d3dRaster->resourse )
        CD3D1XTextureMemoryManager::AddNew( d3dRaster->resourse );
    return true;
}

bool CRwD3D1XEngine::RasterDestroy( RwRaster *raster )
{
    auto d3dRaster = GetD3D1XRaster( raster );
    if ( raster&&d3dRaster->resourse )
    {
        CD3D1XTextureMemoryManager::Remove( d3dRaster->resourse );
        d3dRaster->resourse = nullptr;
    }
    return true;
}

// NATIVE TEXTURE FUNCTIONS 

bool CRwD3D1XEngine::NativeTextureRead( RwStream *stream, RwTexture** tex )
{
    _rwNativeTexture textureInfo; RasterFormat rasterInfo;
    unsigned int lengthOut, versionOut; unsigned char savedFormat;
    RwRaster *raster = nullptr;
    RwTexture *texture;

    if ( !RwStreamFindChunk( stream, rwID_STRUCT, &lengthOut, &versionOut ) )
        return false;
    // Currently we read textures differently depending on the renderware version
    // TODO: reduce repeated code, improve to support more formats
    if ( versionOut >= 0x34000 && versionOut <= rwLIBRARYVERSION36003 ) // GTA SA
    {
        if ( RwStreamRead( stream, &textureInfo, sizeof( _rwNativeTexture ) ) != sizeof( _rwNativeTexture ) || textureInfo.platformId != rwID_PCD3D9 ||
             RwStreamRead( stream, &rasterInfo, sizeof( RasterFormat ) ) != sizeof( RasterFormat ) )
        {
            g_pDebug->printError( "Error while reading native texture" );
            return false;
        }

        if ( rasterInfo.compressed )
        {
            if ( rasterInfo.d3dFormat == D3DFORMAT::D3DFMT_DXT5 )
            {
                rasterInfo.rasterFormat = rwRASTERFORMAT8888;
            }

            raster = RwRasterCreate( rasterInfo.width, rasterInfo.height, rasterInfo.depth, rasterInfo.rasterFormat | rasterInfo.rasterType | rwRASTERDONTALLOCATE | ( rasterInfo.numLevels > 1 ? rwRASTERFORMATMIPMAP : 0 ) );
            if ( raster == nullptr )
                return false;
        }
        else
        {
            if ( !rasterInfo.cubeTexture )
            {
                raster = RwRasterCreate( rasterInfo.width, rasterInfo.height, rasterInfo.depth, rasterInfo.rasterFormat | rasterInfo.rasterType | ( rasterInfo.numLevels > 1 ? rwRASTERFORMATMIPMAP : 0 ) );
                if ( raster == nullptr )
                    return false;
            }
            else
            {
                // Raster stays null on this branch, so the operation has to stop here: the format
                // save below reads it.
                g_pDebug->printError( "Cubemap textures is unsupported in this version of RenderHook; the texture read is refused." );
                return false;
            }
        }

        // TODO: Add support for palette formats in GTA SA
        /*
        if (rasterInfo.rasterFormat & rwRASTERFORMATPAL4)
        {
        if (RwStreamRead(stream, RwRasterLockPalette(raster, rwRASTERLOCKWRITE), 128) != 128)
        {
        RwRasterUnlockPalette(raster);
        RwRasterDestroy(raster);
        return false;
        }
        }
        else if (rasterInfo.rasterFormat & rwRASTERFORMATPAL8)
        {
        if (RwStreamRead(stream, RwRasterLockPalette(raster, rwRASTERLOCKWRITE), 1024) != 1024)
        {
        RwRasterUnlockPalette(raster);
        RwRasterDestroy(raster);
        return false;
        }
        }
        RwRasterUnlockPalette(raster);*/

        savedFormat = raster->cFormat;

        for ( int i = 0; i < 1; i++ )
        {
            for ( RwUInt8 j = 0; j < rasterInfo.numLevels; j++ )
            {
                if ( RwStreamRead( stream, &lengthOut, sizeof( RwUInt32 ) ) == sizeof( RwUInt32 ) )
                {
                    if ( RwStreamRead( stream, RwRasterLock( raster, j, rwRASTERLOCKWRITE ), lengthOut ) == lengthOut )
                    {
                        RwRasterUnlock( raster );
                        continue;
                    }
                }
                RwRasterUnlock( raster );
                RwRasterDestroy( raster );
                return false;
            }
        }

        raster->cFormat = savedFormat;

        texture = RwTextureCreate( raster );
        if ( !texture )
        {
            RwRasterDestroy( raster );
            return false;
        }
        RwTextureSetFilterModeMacro( texture, textureInfo.filterMode );
        RwTextureSetAddressingUMacro( texture, textureInfo.uAddressing );
        RwTextureSetAddressingVMacro( texture, textureInfo.vAddressing );
        RwTextureSetName( texture, textureInfo.name );
        RwTextureSetMaskName( texture, textureInfo.mask );
        if ( texture->raster )
        {
            auto d3draster = GetD3D1XRaster( texture->raster );
            if ( d3draster&&d3draster->resourse )
            {
                char* texName = textureInfo.name;
                d3draster->resourse->SetDebugName( texName );
            }
        }
        *tex = texture;
    }
    else
    {
        if ( RwStreamRead( stream, &textureInfo, sizeof( _rwNativeTexture ) ) != sizeof( _rwNativeTexture ) || textureInfo.platformId != rwID_PCD3D8 ||
             RwStreamRead( stream, &rasterInfo, sizeof( RasterFormat ) ) != sizeof( RasterFormat ) )
            return false;
        if ( rasterInfo.compression )
        {
            raster = RwRasterCreate( rasterInfo.width, rasterInfo.height, rasterInfo.depth, rasterInfo.rasterFormat | rasterInfo.rasterType | rwRASTERDONTALLOCATE | ( rasterInfo.numLevels > 1 ? rwRASTERFORMATMIPMAP : 0 ) );
            if ( !raster )
                return false;
        }
        else
        {
            raster = RwRasterCreate( rasterInfo.width, rasterInfo.height, rasterInfo.depth, rasterInfo.rasterFormat | rasterInfo.rasterType | ( rasterInfo.numLevels > 1 ? rwRASTERFORMATMIPMAP : 0 ) );
            if ( !raster )
                return false;
        }
        //raster->cFlags ^= rwRASTERDONTALLOCATE;

        // 
        if ( rasterInfo.rasterFormat & rwRASTERFORMATPAL4 )
        {
            g_pDebug->printError( "RW PAL4 raster conversion is not implemented; the raster is left unconverted." );
        }
        else if ( rasterInfo.rasterFormat & rwRASTERFORMATPAL8 )
        {
            /*auto d3draster = GetD3D1XRaster(raster)->resourse;
            if (RwStreamRead(stream, &d3draster->GetPalettePtr()[0], 1024) != 1024)
            {
                RwRasterDestroy(raster);
                return false;
            }*/
        }

        savedFormat = raster->cFormat;

        for ( int i = 0; i < 1; i++ )
        {
            for ( RwUInt8 j = 0; j < rasterInfo.numLevels; j++ )
            {
                if ( RwStreamRead( stream, &lengthOut, sizeof( RwUInt32 ) ) == sizeof( RwUInt32 ) )
                {
                    if ( RwStreamRead( stream, RwRasterLock( raster, j, rwRASTERLOCKWRITE ), lengthOut ) == lengthOut )
                    {

                        RwRasterUnlock( raster );
                        continue;
                    }
                }
                RwRasterUnlock( raster );
                RwRasterDestroy( raster );
                return false;
            }
        }

        raster->cFormat = savedFormat;

        texture = RwTextureCreate( raster );
        if ( !texture )
        {
            RwRasterDestroy( raster );
            return false;
        }
        RwTextureSetFilterModeMacro( texture, textureInfo.filterMode );
        RwTextureSetAddressingUMacro( texture, textureInfo.uAddressing );
        RwTextureSetAddressingVMacro( texture, textureInfo.vAddressing );
        RwTextureSetName( texture, textureInfo.name );
        RwTextureSetMaskName( texture, textureInfo.mask );
        *tex = texture;
    }

    return true;
}

bool CRwD3D1XEngine::RasterLock( RwRaster *raster, UINT flags, void** data )
{
    UINT level = flags >> 8;
    RwD3D1XRaster* d3dRaster = GetD3D1XRaster( raster );

    // If we already had locked this raster we shouldn't continue.
    if ( raster->cpPixels != nullptr )
        return false;

    d3dRaster->lockFlags = static_cast<RwUInt8>( level );

    // Keep original texture size and calculate new texture size by taking square root.
    raster->originalWidth = raster->width;
    raster->originalHeight = raster->height;
    raster->width = max( raster->width >> level, 1 );
    raster->height = max( raster->height >> level, 1 );

    // Calculate stride (bytes per line) for BC1 compression we have 8 byte blocks, for BC2+ 16 byte blocks.
    if ( d3dRaster->format == DXGI_FORMAT_BC1_UNORM )
        raster->stride = ( ( raster->width + 3 ) / 4 ) * 8;
    else if ( d3dRaster->format == DXGI_FORMAT_BC2_UNORM || d3dRaster->format == DXGI_FORMAT_BC3_UNORM )
        raster->stride = ( ( ( raster->width + 3 ) / 4 ) ) * 16;
    else
        raster->stride = ( raster->width * 32 + 7 ) / 8;

    // Lower bound is 64 to avoid textures smaller than 8x8.
    size_t pixelCount = max( static_cast<size_t>( raster->stride*raster->height ), 64 );
    raster->cpPixels = (RwUInt8*)malloc( pixelCount );

    *data = (void*)raster->cpPixels;
    if ( flags&rwRASTERLOCKREAD /*|| d3dRaster->resourse->hasPalette()*/ )
    {
        *data = d3dRaster->resourse->LockToRead();
    }
    return true;
}

bool CRwD3D1XEngine::RasterUnlock( RwRaster *raster )
{
    RwD3D1XRaster* d3dRaster = GetD3D1XRaster( raster );
    if ( raster->cpPixels == nullptr )
        return false;
    if ( ( raster->width > 4 ) && ( raster->height > 4 ) )
    {
        auto d3dtexture = d3dRaster->resourse;
        if ( !d3dtexture->IsLockedToRead() )
            m_pRenderer->getContext()->UpdateSubresource( d3dtexture->GetResource(), d3dRaster->lockFlags, nullptr, raster->cpPixels, raster->stride, 0 );
        else
            d3dtexture->UnlockFromRead();
    }
    free( raster->cpPixels );
    raster->cpPixels = nullptr;
    raster->stride = 0;
    raster->width = raster->originalWidth;
    raster->height = raster->originalHeight;
    d3dRaster->lockFlags = 0;
    return true;
}

bool CRwD3D1XEngine::CameraClear( RwCamera *camera, RwRGBA *color, RwInt32 flags )
{
    m_pRenderer->Clear( camera, *color, flags );
    return true;
}


bool CRwD3D1XEngine::CameraBeginUpdate( RwCamera *camera )
{
    RwProcessorForceSinglePrecision();
    dgGGlobals = camera;
    // View and projection transform computing
    RwGraphicsMatrix *viewTransformRef = (RwGraphicsMatrix*)&RwD3D9D3D9ViewTransform;
    RwGraphicsMatrix *projTransformRef = (RwGraphicsMatrix*)&RwD3D9D3D9ProjTransform;
    RwMatrix* camMatrix = RwFrameGetLTM( static_cast<RwFrame*>( camera->object.object.parent ) );
    // rw used inverse of camera frame matrix with negative column-vector
    RwMatrixInvert( (RwMatrix*)viewTransformRef, camMatrix );
    viewTransformRef->m[0].x = -viewTransformRef->m[0].x;
    viewTransformRef->m[1].x = -viewTransformRef->m[1].x;
    viewTransformRef->m[2].x = -viewTransformRef->m[2].x;
    viewTransformRef->m[3].x = -viewTransformRef->m[3].x;
    RwV3d* up = RwMatrixGetUp( camMatrix );
    RwV3d* right = RwMatrixGetRight( camMatrix );

    viewTransformRef->m[0].w = 0.0f;
    viewTransformRef->m[1].w = 0.0f;
    viewTransformRef->m[2].w = 0.0f;
    viewTransformRef->m[3].w = 1.0f;

    // construct projection matrix, transforms into clip-space
    projTransformRef->m[0].x = camera->recipViewWindow.x;
    projTransformRef->m[1].y = camera->recipViewWindow.y;

    projTransformRef->m[2].x = ( camera->viewOffset.x ) * camera->recipViewWindow.x;
    projTransformRef->m[2].y = ( camera->viewOffset.y ) * camera->recipViewWindow.y;

    projTransformRef->m[3].x = -( ( camera->viewOffset.x ) * camera->recipViewWindow.x );
    projTransformRef->m[3].y = -( ( camera->viewOffset.y ) * camera->recipViewWindow.y );

    const bool reversedZ = IsReversedZCamera( camera );
    if ( camera->projectionType == rwPARALLEL )
    {
        projTransformRef->m[2].z = 1.0f / ( camera->farPlane - camera->nearPlane );
        projTransformRef->m[2].w = 0;
        projTransformRef->m[3].w = 1.0f;
    }
    else
    {
        projTransformRef->m[2].z = camera->farPlane / ( camera->farPlane - camera->nearPlane );
        projTransformRef->m[2].w = 1.0;
        projTransformRef->m[3].w = 0;
    }
    projTransformRef->m[3].z = -( projTransformRef->m[2].z * camera->nearPlane );
    if ( reversedZ )
    {
        // Remap standard [0, 1] depth to [1, 0] (near = 1, far = 0) so float depth retains far-distance precision.
        projTransformRef->m[2].z = projTransformRef->m[2].w - projTransformRef->m[2].z;
        projTransformRef->m[3].z = projTransformRef->m[3].w - projTransformRef->m[3].z;
    }
    g_pStateMgr->SetReversedZ( reversedZ );
    // compute view-projection transform
    RwD3D9ActiveViewProjTransform = nullptr;
    g_pRenderBuffersMgr->UpdateViewProjMatricles( *(RwMatrix*)viewTransformRef, *(RwMatrix*)projTransformRef );

    // resize main framebuffer if adapter mode changed
    bool bResized = false;
    // compare adapter mode dimensions against renderer and camera raster dimensions
    DXGI_MODE_DESC currModeDesc = m_pRenderer->getCurrentAdapterModeDesc();
    if ( camera->frameBuffer && RwRasterGetType( camera->frameBuffer ) != rwRASTERTYPECAMERATEXTURE )
    {
        // If we have different client window sizes we must resize it.
        if ( m_pRenderer->getWidth() != currModeDesc.Width || m_pRenderer->getHeight() != currModeDesc.Height )
        {
            auto swapChain = m_pRenderer->getSwapChain();
            m_pRenderer->setSize( currModeDesc.Width, currModeDesc.Height );
            g_pStateMgr->SetScreenSize( static_cast<float>( currModeDesc.Width ), static_cast<float>( currModeDesc.Height ) );
            if ( camera->zBuffer )
            {
                camera->zBuffer->width  = currModeDesc.Width;
                camera->zBuffer->height = currModeDesc.Height;
                m_pRastersToReload.push_back( camera->zBuffer );
            }
            RsGlobal.maximumWidth  = currModeDesc.Width;
            RsGlobal.maximumHeight = currModeDesc.Height;
#ifdef USE_ANTTWEAKBAR
            TwWindowSize( currModeDesc.Width, currModeDesc.Height );
#endif
            RwRasterDestroy( camera->frameBuffer );
            swapChain->ResizeTarget( &currModeDesc );
            swapChain->ResizeBuffers( 1, currModeDesc.Width, currModeDesc.Height, currModeDesc.Format, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH );
            camera->frameBuffer = RwRasterCreate( currModeDesc.Width, currModeDesc.Height, 32, rwRASTERDONTALLOCATE | rwRASTERTYPECAMERA );
            m_bScreenSizeChanged = true;
            bResized             = true;

            if ( gDebugSettings.GetToggleField( "Windowed" ) )
            {
                SetWindowLongPtr( m_pRenderer->getHWND(), GWL_STYLE, WS_POPUP | WS_VISIBLE );
                SetWindowLongPtr( m_pRenderer->getHWND(), GWL_EXSTYLE, 0 );
            }
            SetWindowPos( m_pRenderer->getHWND(), nullptr, 0, 0, currModeDesc.Width, currModeDesc.Height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED );
        }
        if ( m_bScreenSizeChanged )
            ReloadTextures();
    }
    if ( camera->frameBuffer && camera->frameBuffer->cType == rwRASTERTYPECAMERATEXTURE )
    {
        //RwD3D1XRaster* d3dRaster = GetD3D1XRaster(camera->frameBuffer);
        //d3dRaster->resourse->BeginRendering();
    }

    m_pRenderer->BeginUpdate( camera );
    if ( bResized && camera->zBuffer )
    {
        RwRGBA clearCol = { 0, 0, 0, 255 };
        m_pRenderer->Clear( camera, clearCol, rwCAMERACLEARZ | rwCAMERACLEARSTENCIL );
    }
    return true;
}

bool CRwD3D1XEngine::CameraEndUpdate( RwCamera *camera )
{
    RwD3D1XRaster* d3dRaster = GetD3D1XRaster( camera->frameBuffer );
    if ( camera->frameBuffer && camera->frameBuffer->cType == rwRASTERTYPECAMERATEXTURE )
        //d3dRaster->resourse->EndRendering();
        m_pRenderer->EndUpdate( camera );
    dgGGlobals = nullptr;
    g_pStateMgr->SetReversedZ( false );
    return true;
}

bool CRwD3D1XEngine::IsReversedZCamera( RwCamera *camera )
{
    // Restrict reversed Z to the main scene camera so shadow maps and auxiliary passes keep standard depth comparisons.
    return camera != nullptr && camera == Scene.m_pRwCamera;
}

bool CRwD3D1XEngine::RasterShowRaster( RwRaster *raster, UINT flags )
{
    m_pRenderer->Present( flags & 1 );
    return true;
}

void destroyNotify( RwResEntry * resEntry )
{
    RxD3D9ResEntryHeader resEntryHeader = static_cast<RxInstanceData*>( resEntry )->header;
    if ( resEntryHeader.indexBuffer )
    {
        CD3D1XIndexBuffer* buffptr = static_cast<CD3D1XIndexBuffer*>( resEntryHeader.indexBuffer );
        delete buffptr;
        resEntryHeader.indexBuffer = nullptr;
    }
    if ( resEntryHeader.vertexStream[0].vertexBuffer )
    {
        CD3D1XVertexBuffer* buffptr = static_cast<CD3D1XVertexBuffer*>( resEntryHeader.vertexStream[0].vertexBuffer );
        CD3D1XVertexBufferManager::Remove( buffptr );
        resEntryHeader.vertexStream[0].vertexBuffer = nullptr;
    }
    if ( resEntryHeader.vertexDeclaration )
    {
        //((ID3D11Buffer*)(resEntryHeader.vertexDeclaration))->Release();
        resEntryHeader.vertexDeclaration = nullptr;
    }
}

void destroyNotifySkin( RwResEntry * resEntry )
{
    RxD3D9ResEntryHeader resEntryHeader = ( (RxInstanceData*)resEntry )->header;
    if ( resEntryHeader.indexBuffer )
    {
        CD3D1XIndexBuffer* buffptr = static_cast<CD3D1XIndexBuffer*>( resEntryHeader.indexBuffer );
        delete buffptr;
        resEntryHeader.indexBuffer = nullptr;
    }
    if ( resEntryHeader.vertexStream[0].vertexBuffer )
    {
        CD3D1XVertexBuffer* buffptr = static_cast<CD3D1XVertexBuffer*>( resEntryHeader.vertexStream[0].vertexBuffer );
        CD3D1XVertexBufferManager::Remove( buffptr );
        resEntryHeader.vertexStream[0].vertexBuffer = nullptr;
    }
    if ( resEntryHeader.vertexDeclaration )
    {
        //((ID3D11Buffer*)(resEntryHeader.vertexDeclaration))->Release();
        resEntryHeader.vertexDeclaration = nullptr;
    }
}

int SortTriangles( const void* a, const void* b )
{
    return rwD3D9SortTriangles( a, b );
}

bool CRwD3D1XEngine::AtomicAllInOneNode( RxPipelineNode *self, const RxPipelineNodeParam *params )
{
    RpAtomic* atomic = (RpAtomic*)params->dataParam;
    RpGeometry* geom = atomic->geometry;
    RxInstanceData* entryData = nullptr;
    RxD3D9Pipelines* callbacks = (RxD3D9Pipelines*)self->privateData;


    if ( geom->numVertices <= 0 )
        return true;

    RpD3DMeshHeader* mesh = (RpD3DMeshHeader*)geom->mesh;
    if ( mesh->numMeshes <= 0 )
        return true;

    RwUInt32 flags = geom->flags;
    if ( ( flags & rpGEOMETRYNATIVEFLAGSMASK ) != rpGEOMETRYNATIVE )
    {
        // If geometry has morph target we use it's resource entry, otherwise atomic one
        if ( geom->numMorphTargets == 1 )
            entryData = (RxInstanceData*)geom->repEntry;
        else
            entryData = (RxInstanceData*)atomic->repEntry;

        if ( entryData )
        {
            if ( entryData->header.serialNumber != mesh->serialNum )
            {
                _RwResourcesFreeResEntry( entryData );
                entryData = nullptr;
            }
        }
        if ( entryData )
        {
            if ( geom->lockedSinceLastInst ||
                 geom->numMorphTargets != 1 )
            {
                auto reinstance = callbacks->reinstance;
                if ( reinstance && !reinstance( atomic, entryData, callbacks->instance ) )
                {
                    _RwResourcesFreeResEntry( entryData );
                    return false;
                }
                atomic->interpolator.flags &= ~rpINTERPOLATORDIRTYINSTANCE;
                geom->lockedSinceLastInst = 0;
            }
            //_RwResourcesUseResEntry(entryData);
            if ( entryData->link.next )
            {
                rwLinkListRemoveLLLink( &entryData->link );
                const RwModuleInfo ResModule = RpResModule;
                UINT engineOffset = reinterpret_cast<UINT>( *static_cast<RwGlobals**>( RwEngineInstance ) );
                auto globalPtr = reinterpret_cast<rwResourcesGlobals*>( engineOffset + ResModule.globalsOffset );
                rwLinkListAddLLLink( globalPtr->res.usedEntries, &entryData->link );
            }
        }
        else
        {
            if ( geom->numMorphTargets == 1 )
                entryData = m_D3DInstance( atomic, geom, 1, &geom->repEntry, mesh, callbacks->instance, ( rpGEOMETRYNATIVEINSTANCE & flags ) != 0 );
            else
                entryData = m_D3DInstance( atomic, atomic, 1, &atomic->repEntry, mesh, callbacks->instance, ( rpGEOMETRYNATIVEINSTANCE & flags ) != 0 );
            if ( entryData == nullptr )
                return false;
            geom->lockedSinceLastInst = 0;
        }
    }
    else
        entryData = static_cast<RxInstanceData*>( geom->repEntry );

    if ( flags & rpGEOMETRYNATIVEINSTANCE )
        return true;
    {
        auto ltm = RwFrameGetLTM( static_cast<RwFrame*>( atomic->object.object.parent ) );
        g_pRenderBuffersMgr->UpdateWorldMatrix( ltm );
        g_pRenderBuffersMgr->SetMatrixBuffer();
        if ( callbacks->render )
            callbacks->render( entryData, atomic, rpATOMIC, geom->flags );
    }

    return true;
}

void CRwD3D1XEngine::DefaultRenderCallback( RwResEntry *repEntry, void *object, RwUInt8 type, RwUInt32 flags )
{
    m_pDefaultPipe->Render( repEntry, object, type, flags );
}

RwBool CRwD3D1XEngine::DefaultInstanceCallback( void *object, RxD3D9ResEntryHeader *resEntryHeader, RwBool reinstance )
{
    return m_pDefaultPipe->Instance( object, resEntryHeader, reinstance );
}

RwBool CRwD3D1XEngine::Im3DSubmitNode()
{
    rwIm3DPool* pool = rwD3D9ImmPool;
    if ( pool == nullptr )
        return true;
    if ( pool->stash.flags&rwIM3D_ALLOPAQUE )
        g_pStateMgr->SetAlphaBlendEnable( false );
    else
        g_pStateMgr->SetAlphaBlendEnable( true );
    g_pRenderBuffersMgr->UpdateWorldMatrix( pool->stash.ltm );
    g_pRenderBuffersMgr->SetMatrixBuffer();
    return m_pIm3DPipe->SubmitNode();
}


RxInstanceData * CRwD3D1XEngine::m_D3DInstance( void * object, void * owner,
                                                RwUInt8 type, RwResEntry ** resEntryPointer, RpD3DMeshHeader * mesh, RxD3D9AllInOneInstanceCallBack instanceCallback, int allocateNative )
{
    // used decompiler output as base and
    // now https://github.com/GTAmodding/rw37/blob/f3fb1bb4b5dfbb65e9ba60746aebaf4cb0c7708e/src/world/pipe/p2/d3d9/D3D9pipe.c as reference
    bool	convertToTriList = false,
        createIndexBuffer = false;
    RxInstanceData *entry;
    RpAtomic* atomic = (RpAtomic*)object;
    auto size = sizeof( RxD3D9InstanceData ) * mesh->numMeshes + sizeof( RxD3D9ResEntryHeader );

    if ( allocateNative )
    {
        entry = (RxInstanceData*)_RwMalloc( sizeof( RwResEntry ) + size,
                                            rwMEMHINTDUR_EVENT | rwID_WORLDPIPEMODULE );
        *resEntryPointer = entry;
        entry->link.next = nullptr;
        entry->link.prev = nullptr;
        entry->size = size;
        entry->owner = nullptr;
        entry->ownerRef = nullptr;
        entry->destroyNotify = destroyNotify;
    }
    else
        entry = (RxInstanceData*)_RwResourcesAllocateResEntry( owner, resEntryPointer, size, destroyNotify );

    memset( &entry->header, 0, size );
    entry->header.serialNumber = mesh->serialNum;
    entry->header.numMeshes = mesh->numMeshes;
    entry->header.indexBuffer = nullptr;
    entry->header.totalNumIndex = 0;

    auto primType = mesh->flags & rpMESHHEADERPRIMMASK;

    if ( ( mesh->flags & rpMESHHEADERUNINDEXED ) == 0 )
    {

        for ( auto i = 0; i < mesh->numMeshes; i++ )
            entry->header.totalNumIndex += GetMeshesData( mesh )[i].numIndices;

        if ( entry->header.totalNumIndex > 0 )
        {
            createIndexBuffer = true;
            if ( primType == rpMESHHEADERTRISTRIP )
            {
                RwUInt32 numTriangles = 0xffffffff / 3;
                if ( type == (RwUInt8)rwSECTORATOMIC )
                {
                    RpWorldSector *sector = (RpWorldSector *)object;
                    numTriangles = sector->numTriangles;
                }
                else if ( type == rpATOMIC )
                {
                    numTriangles = atomic->geometry->numTriangles;
                }
                // renderware converts inefficient tristrips to trilists to reduce primitive count
                if ( entry->header.totalNumIndex > numTriangles * 3 )
                {
                    convertToTriList = true;
                    entry->header.totalNumIndex = 3 * numTriangles;
                }
            }
        }
    }

    if ( convertToTriList )
        entry->header.primType = rwPRIMTYPETRILIST;
    else
        entry->header.primType = (RwPrimitiveType)CD3D1XEnumParser::ConvertPrimTopology( (RpMeshHeaderFlags)primType );

    /* Initialize the vertex buffers pointers */
    for ( int n = 0; n < RWD3D9_MAX_VERTEX_STREAMS; n++ )
    {
        entry->header.vertexStream[n].vertexBuffer = nullptr;
        entry->header.vertexStream[n].offset = 0;
        entry->header.vertexStream[n].stride = 0;
        entry->header.vertexStream[n].geometryFlags = false;
        entry->header.vertexStream[n].managed = false;
        entry->header.vertexStream[n].dynamicLock = false;
    }

    entry->header.vertexDeclaration = nullptr;

    std::vector<RxVertexIndex> indexBufferData( entry->header.totalNumIndex * 3 );

    unsigned int firstVert = 0;
    unsigned int startIndex = 0;

    //	Load index buffer data.
    if ( mesh->numMeshes > 0 )
    {
        RxD3D9InstanceData* meshData = nullptr;
        unsigned int currentIndexOffset = 0;
        for ( unsigned int i = 0; i < mesh->numMeshes; i++ )
        {
            meshData = &GetModelsData( entry )[i];
            auto meshData2 = GetMeshesData( mesh )[i];
            RwUInt32	minVert = 0, numVert = 0;
            if ( mesh->flags & rpMESHHEADERPOINTLIST )
            {
                numVert = meshData2.numIndices;
                minVert = currentIndexOffset;
            }
            else if ( meshData2.numIndices > 0 )
            {
                minVert = UINT_MAX;
                UINT maxIndex = 0;
                for ( unsigned int j = 0; j < meshData2.numIndices; j++ )
                {
                    minVert = min( minVert, meshData2.indices[j] );
                    maxIndex = max( maxIndex, meshData2.indices[j] );
                }
                numVert = maxIndex - minVert + 1;
            }

            meshData->numVertices = numVert;
            meshData->minVert = minVert;
            meshData->material = meshData2.material;
            meshData->vertexShader = nullptr;
            meshData->vertexAlpha = false;

            if ( currentIndexOffset > indexBufferData.size() )
            {
                meshData->numIndex = 0;
                meshData->startIndex = 0;
            }
            else
            {
                meshData->numIndex = meshData2.numIndices;
                meshData->startIndex = currentIndexOffset;
                if ( convertToTriList )
                {
                    meshData->numIndex = rwD3D9ConvertToTriList( &indexBufferData[currentIndexOffset], meshData2.indices, meshData2.numIndices, meshData->minVert );
                }
                else if ( meshData->minVert > 0 )
                {
                    for ( size_t j = 0; j < meshData2.numIndices; j++ )
                        indexBufferData[currentIndexOffset + j] = meshData2.indices[j] - meshData->minVert;
                }
                else
                    memcpy( &indexBufferData[currentIndexOffset], meshData2.indices, meshData2.numIndices * sizeof( RxVertexIndex ) );
                if ( entry->header.primType == rwPRIMTYPETRILIST )
                    qsort( &indexBufferData[currentIndexOffset], meshData->numIndex / 3, 6u, SortTriangles );
                currentIndexOffset += static_cast<size_t>( meshData->numIndex );

            }
            switch ( entry->header.primType )
            {
            case rwPRIMTYPEPOLYLINE:
                meshData->numPrimitives = meshData2.numIndices >> 1;// numPrimitives
                break;
            case rwPRIMTYPETRILIST:
                meshData->numPrimitives = meshData2.numIndices - 1;// numPrimitives
                break;
            case rwPRIMTYPETRISTRIP:
                meshData->numPrimitives = meshData2.numIndices / 3;// numPrimitives
                break;
            case rwPRIMTYPETRIFAN:
            case rwPRIMTYPEPOINTLIST:
                meshData->numPrimitives = meshData2.numIndices - 2;// numPrimitives
                break;
            default:
                meshData->numPrimitives = 0;          // numPrimitives
                break;
            }
            meshData->baseIndex = 0;
        }
    }

    if ( createIndexBuffer )
    {
        D3D11_SUBRESOURCE_DATA	InitData = {};
        InitData.pSysMem = indexBufferData.data();
        entry->header.indexBuffer = new CD3D1XIndexBuffer( (unsigned int)indexBufferData.size(), &InitData );
    }
    else
        entry->header.indexBuffer = nullptr;

    if ( instanceCallback != nullptr )
    {
        if ( !instanceCallback( object, &entry->header, 0 ) )
        {
            if ( allocateNative )
                _RwFree( entry );
            else
                _RwResourcesFreeResEntry( entry );
            return nullptr;
        }
    }
    return entry;
}

RxInstanceData * CRwD3D1XEngine::m_D3DSkinInstance( void * object, void * instanceObject, RwResEntry ** repEntry, RpD3DMeshHeader * mesh )
{
    bool	convertToTriList = false,
        createIndexBuffer = false;
    RxInstanceData *entry;
    RpAtomic* atomic = (RpAtomic*)object;
    size_t size = sizeof( RxD3D9InstanceData ) * mesh->numMeshes + sizeof( RxD3D9ResEntryHeader );
    entry = (RxInstanceData*)_RwResourcesAllocateResEntry( instanceObject, repEntry, size, destroyNotifySkin );

    memset( &entry->header, 0, size );
    entry->header.serialNumber = mesh->serialNum;
    entry->header.numMeshes = mesh->numMeshes;
    entry->header.indexBuffer = 0;
    entry->header.totalNumIndex = 0;
    for ( auto i = 0; i < mesh->numMeshes; i++ )
        entry->header.totalNumIndex += GetMeshesData( mesh )[i].numIndices;
    if ( entry->header.totalNumIndex > 0 )
        createIndexBuffer = true;
    if ( ( mesh->flags & rpMESHHEADERPRIMMASK ) == rpMESHHEADERTRISTRIP && atomic->geometry->numTriangles * 3 > 0 )
    {
        convertToTriList = true;
        entry->header.totalNumIndex = atomic->geometry->numTriangles * 3;
    }
    if ( convertToTriList )
        entry->header.primType = rwPRIMTYPETRILIST;
    else if ( ( mesh->flags & rpMESHHEADERPRIMMASK ) == rpMESHHEADERTRISTRIP )
        entry->header.primType = rwPRIMTYPETRISTRIP;
    else if ( ( mesh->flags & rpMESHHEADERPRIMMASK ) == rpMESHHEADERTRIFAN )
        entry->header.primType = rwPRIMTYPETRIFAN;
    else if ( ( mesh->flags & rpMESHHEADERPRIMMASK ) == rpMESHHEADERLINELIST )
        entry->header.primType = rwPRIMTYPELINELIST;
    else if ( ( mesh->flags & rpMESHHEADERPRIMMASK ) == rpMESHHEADERPOLYLINE )
        entry->header.primType = rwPRIMTYPEPOLYLINE;
    else if ( ( mesh->flags & rpMESHHEADERPRIMMASK ) == rpMESHHEADERPOINTLIST )
        entry->header.primType = rwPRIMTYPEPOINTLIST;
    else if ( ( mesh->flags & rpMESHHEADERPRIMMASK ) == 0 )
        entry->header.primType = rwPRIMTYPETRILIST;

    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.ByteWidth = entry->header.totalNumIndex * sizeof( RxVertexIndex );
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;

    std::vector<RxVertexIndex> indexBufferData( entry->header.totalNumIndex );
    //	Load index buffer data.
    if ( mesh->numMeshes > 0 )
    {
        RxD3D9InstanceData* meshData = nullptr;
        size_t currentIndexOffset = 0;
        for ( size_t i = 0; i < mesh->numMeshes; i++ )
        {
            meshData = &GetModelsData( entry )[i];
            auto meshData2 = GetMeshesData( mesh )[i];
            RwUInt32	minVert = 0,
                numVert = 0;
            if ( mesh->flags & rpMESHHEADERPOINTLIST )
            {
                numVert = meshData2.numIndices;
                minVert = static_cast<UINT>( currentIndexOffset );
            }
            else if ( meshData2.numIndices > 0 )
            {
                minVert = UINT_MAX;
                UINT maxIndex = 0;
                for ( size_t j = 0; j < static_cast<size_t>( meshData2.numIndices ); j++ )
                {
                    minVert = min( minVert, meshData2.indices[j] );
                    maxIndex = max( maxIndex, meshData2.indices[j] );
                }
                numVert = maxIndex - minVert + 1;
            }
            meshData->numVertices = numVert;
            meshData->minVert = minVert;
            meshData->vertexShader = nullptr;
            meshData->material = meshData2.material;
            meshData->vertexAlpha = false;
            if ( &indexBufferData[currentIndexOffset] == nullptr )
            {
                meshData->numIndex = 0;
                meshData->startIndex = 0;
            }
            else
            {
                meshData->numIndex = meshData2.numIndices;
                meshData->startIndex = static_cast<RwUInt32>( currentIndexOffset );
                if ( convertToTriList )
                {
                    meshData->numIndex = rwD3D9ConvertToTriList( &indexBufferData[currentIndexOffset], meshData2.indices, meshData2.numIndices, meshData->minVert );
                }
                else if ( meshData->minVert > 0 )
                {
                    for ( size_t j = 0; j < static_cast<size_t>( meshData2.numIndices ); j++ )
                        indexBufferData[currentIndexOffset + j] = meshData2.indices[j] - static_cast<RwUInt16>( meshData->minVert );
                }
                else
                    memcpy( &indexBufferData[currentIndexOffset], meshData2.indices, static_cast<size_t>( meshData2.numIndices ) * sizeof( RxVertexIndex ) );
                if ( entry->header.primType == rwPRIMTYPETRILIST )
                    qsort( &indexBufferData[currentIndexOffset], meshData->numIndex / 3, 6u, SortTriangles );
                currentIndexOffset += static_cast<size_t>( meshData->numIndex );

            }
            switch ( entry->header.primType )
            {
            case rwPRIMTYPEPOLYLINE:
                meshData->numPrimitives = meshData2.numIndices >> 1;// numPrimitives
                break;
            case rwPRIMTYPETRILIST:
                meshData->numPrimitives = meshData2.numIndices - 1;// numPrimitives
                break;
            case rwPRIMTYPETRISTRIP:
                meshData->numPrimitives = meshData2.numIndices / 3;// numPrimitives
                break;
            case rwPRIMTYPETRIFAN:
            case rwPRIMTYPEPOINTLIST:
                meshData->numPrimitives = meshData2.numIndices - 2;// numPrimitives
                break;
            default:
                meshData->numPrimitives = 0;          // numPrimitives
                break;
            }
            meshData->baseIndex = 0;
        }
    }

    if ( createIndexBuffer )
    {
        D3D11_SUBRESOURCE_DATA	InitData = {};
        InitData.pSysMem = indexBufferData.data();
        entry->header.indexBuffer = new CD3D1XIndexBuffer( (unsigned int)indexBufferData.size(), &InitData );
    }
    RpHAnimHierarchy* atomicHier = AtomicGetHAnimHier( atomic );
    RpSkin* geomSkin = GeometryGetSkin( atomic->geometry );
    if ( geomSkin )
    {
        if ( geomSkin->meshBoneRLECount )
            geomSkin->boneCount = geomSkin->boneLimit;
        else
            geomSkin->boneCount = geomSkin->numBoneIds;
        if ( atomicHier )
            geomSkin->useVS = true;
        else
            geomSkin->useVS = false;
    }
    entry->header.totalNumVertex = atomic->geometry->numVertices;
    if ( m_pSkinPipe->Instance( atomic, &entry->header, false ) )
        return entry;
    return nullptr;
}

void CRwD3D1XEngine::SetTexture( RwTexture * tex, int Stage )
{
    if ( tex == nullptr )
    {
        if ( Stage == 0 )
            g_pStateMgr->SetRaster( nullptr );
        else
            g_pStateMgr->SetRaster( nullptr, Stage );
        return;
    }
    if ( Stage == 0 )
    {
        RenderStateSet( rwRENDERSTATETEXTUREFILTER, RwTextureGetFilterMode( tex ) );
        RenderStateSet( rwRENDERSTATETEXTUREADDRESSU, RwTextureGetAddressingU( tex ) );
        RenderStateSet( rwRENDERSTATETEXTUREADDRESSV, RwTextureGetAddressingV( tex ) );
        g_pStateMgr->SetTextureAnisotropy( RpAnisotTextureGetMaxAnisotropy( tex ) );
    }
    auto raster = RwTextureGetRaster( tex );
    if ( Stage == 0 )
        g_pStateMgr->SetRaster( raster );
    else
        g_pStateMgr->SetRaster( raster, Stage );
}

bool CRwD3D1XEngine::SkinAllInOneNode( RxPipelineNode * self, const RxPipelineNodeParam * params )
{
    RpAtomic* atomic = (RpAtomic*)params->dataParam;
    RpGeometry* geom = atomic->geometry;
    RxInstanceData* entryData = nullptr;
    RpSkinPipeCB* callbacks = (RpSkinPipeCB*)self->privateData;
    if ( geom->numVertices <= 0 )
        return true;
    RpD3DMeshHeader* mesh = (RpD3DMeshHeader*)geom->mesh;
    if ( !mesh->numMeshes )
        return true;

    if ( geom->numMorphTargets == 1 )
        entryData = (RxInstanceData*)geom->repEntry;
    else
        entryData = (RxInstanceData*)atomic->repEntry;

    if ( entryData == nullptr || entryData->header.serialNumber != mesh->serialNum )
    {
        if ( entryData != nullptr )
            _RwResourcesFreeResEntry( entryData );
        if ( geom->numMorphTargets == 1 )
            entryData = m_D3DSkinInstance( atomic, geom, &geom->repEntry, mesh );
        else
            entryData = m_D3DSkinInstance( atomic, atomic, &atomic->repEntry, mesh );
        if ( entryData == nullptr )
            return false;
        geom->lockedSinceLastInst = 0;
    }
    else
    {
        if ( geom->lockedSinceLastInst )
        {
            m_pSkinPipe->Instance( atomic, &entryData->header, true );
            geom->lockedSinceLastInst = 0;
        }
        atomic->interpolator.flags &= ~1u;
        if ( entryData->link.next )
        {
            rwLinkListRemoveLLLink( &entryData->link );
            RwModuleInfo ResModule = RpResModule;
            UINT engineOffset = ( UINT )*(RwGlobals**)RwEngineInstance;
            rwResourcesGlobals* globalPtr = (rwResourcesGlobals*)( engineOffset + ResModule.globalsOffset );
            rwLinkListAddLLLink( globalPtr->res.usedEntries, &entryData->link );
        }
    }
    {
        RwMatrix *ltm = RwFrameGetLTM( (RwFrame*)atomic->object.object.parent );
        g_pRenderBuffersMgr->UpdateWorldMatrix( ltm );

        g_pRenderBuffersMgr->SetMatrixBuffer();
        if ( callbacks->render )
            m_pSkinPipe->Render( entryData, atomic, 1, geom->flags );
    }
    return true;
}

void CRwD3D1XEngine::SetRenderTargets( RwRaster ** rasters, RwRaster* zBuffer, RwUInt32 rasterCount )
{
    // Every index below is read through the caller's array, so an absent array is the same as zero targets.
    if ( rasters == nullptr )
        rasterCount = 0;

    auto context = m_pRenderer->getContext();
    // Fixed-size instead of a vector: this runs several times per frame and per shadow cascade, and the binding count is
    // bounded by what Direct3D itself accepts.
    ID3D11RenderTargetView * pRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    RwUInt32 boundCount = 0;
    for ( RwUInt32 i = 0; i < rasterCount && boundCount < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++ )
    {
        if ( rasters[i] != nullptr )
        {
            RwD3D1XRaster* pd3dRaster = GetD3D1XRaster( rasters[i] );
            auto renderTarget = dynamic_cast<ID3D1XRenderTargetViewable*>( pd3dRaster->resourse );
            pRTVs[boundCount] = renderTarget ? renderTarget->GetRenderTargetView() : nullptr;
        }
        boundCount++;
    }
    if ( rasterCount > 0 && rasters[0] != nullptr )
    {
        D3D11_VIEWPORT vp;
        vp.Width = (float)rasters[0]->width;
        vp.Height = (float)rasters[0]->height;
        vp.MaxDepth = 1;
        vp.MinDepth = 0;
        vp.TopLeftX = 0; vp.TopLeftY = 0;
        g_pStateMgr->SetViewport( vp );
    }
    if ( zBuffer )
    {
        RwD3D1XRaster* pd3dDepthRaster = GetD3D1XRaster( zBuffer );
        // The depth view is bound only when the raster really carries a depth-stencil resource. Anything else binds no
        // depth buffer at all, instead of dereferencing a lookup or a cast that came back empty.
        ID3D11DepthStencilView* depthStencilView = nullptr;
        if ( pd3dDepthRaster != nullptr )
        {
            if ( auto depthTarget = dynamic_cast<CD3D1XDepthStencilTexture*>( pd3dDepthRaster->resourse ) )
                depthStencilView = depthTarget->GetDepthStencilView();
        }
        g_pStateMgr->SetRenderTargets( boundCount, pRTVs, depthStencilView );
    }
    else
        g_pStateMgr->SetRenderTargets( boundCount, pRTVs, nullptr );
    if ( rasterCount >= 1 )
    {
        float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for ( RwUInt32 i = 0; i < boundCount; i++ )
            if ( pRTVs[i] != nullptr )
                context->ClearRenderTargetView( pRTVs[i], clearColor );
    }
}

void CRwD3D1XEngine::SetRenderTargetsAndUAVs( RwRaster ** rasters, RwRaster ** uavs, RwRaster* zBuffer, RwUInt32 rasterCount, RwUInt32 uavCount, RwUInt32 uavStart )
{
    // No colour targets at all is a valid request here: the caller still gets its depth buffer and UAVs bound, which is
    // the reason this entry point exists next to SetRenderTargets. An absent array is therefore the same as zero.
    if ( rasters == nullptr )
        rasterCount = 0;

    ID3D11DepthStencilView* depthStencilRes = nullptr;
    if ( zBuffer )
    {
        RwD3D1XRaster* pd3dDepthRaster = GetD3D1XRaster( zBuffer );
        if ( pd3dDepthRaster )
        {
            if ( auto depthTarget = dynamic_cast<CD3D1XDepthStencilTexture*>( pd3dDepthRaster->resourse ) )
                depthStencilRes = depthTarget->GetDepthStencilView();
        }
    }
    // Slot-preserving like SetRenderTargets: an entry that resolves to nothing keeps its index, so the views after it do
    // not slide down into lower SV_Target numbers.
    ID3D11RenderTargetView * pRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    RwUInt32 boundCount = 0;
    std::vector<ID3D11UnorderedAccessView*> pUAVs;
    for ( RwUInt32 i = 0; i < rasterCount && boundCount < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++ )
    {
        if ( rasters[i] )
        {
            RwD3D1XRaster* pd3dRaster = GetD3D1XRaster( rasters[i] );
            auto renderTarget = dynamic_cast<ID3D1XRenderTargetViewable*>( pd3dRaster->resourse );
            pRTVs[boundCount] = renderTarget ? renderTarget->GetRenderTargetView() : nullptr;
        }
        boundCount++;
    }
    if ( boundCount > 0 && rasters[0] != nullptr )
    {
        D3D11_VIEWPORT vp;
        vp.Width = static_cast<FLOAT>( rasters[0]->width );
        vp.Height = static_cast<FLOAT>( rasters[0]->height );
        vp.MaxDepth = 1;
        vp.MinDepth = 0;
        vp.TopLeftX = 0; vp.TopLeftY = 0;

        g_pStateMgr->SetViewport( vp );
    }
    if ( uavs == nullptr || uavCount == 0 || uavs[0] == nullptr )
    {
        ID3D11UnorderedAccessView* puav = nullptr;

        m_pRenderer->getContext()->OMSetRenderTargetsAndUnorderedAccessViews( boundCount, pRTVs, depthStencilRes,
                                                                              uavStart, 1, &puav, nullptr );
        return;
    }
    /*for (RwUInt32 i = 0; i < uavCount; i++)
    {
        RwD3D1XRaster* pd3dRaster = GetD3D1XRaster(uavs[i]);
        pUAVs.push_back(pd3dRaster->resourse->GetUAV());
    }*/

    m_pRenderer->getContext()->OMSetRenderTargetsAndUnorderedAccessViews( boundCount, pRTVs, depthStencilRes,
                                                                          uavStart, static_cast<UINT>( pUAVs.size() ),
                                                                          pUAVs.data(), nullptr );
    if ( boundCount > 1 )
    {
        float clearColor[] = { 0.0f, 0.0f, 0.0f, 0.0f };
        for ( RwUInt32 i = 0; i < boundCount; i++ )
            if ( pRTVs[i] != nullptr )
                m_pRenderer->getContext()->ClearRenderTargetView( pRTVs[i], clearColor );
    }
}

void CRwD3D1XEngine::ReloadTextures()
{
    if ( !m_pRastersToReload.empty() )
    {
        for ( auto raster : m_pRastersToReload )
        {
            if ( raster )
            {
                auto resource = GetD3D1XRaster( raster )->resourse;
                resource->Reallocate();
            }
        }
        m_pRastersToReload = {};
    }
}

RwRaster * CRwD3D1XEngine::CreateD3D9Raster( RwInt32 width, RwInt32 height, D3DFORMAT format, RwInt32 flags )
{
    RwRaster* raster = RwRasterCreate( width, height, 32, flags | rwRASTERDONTALLOCATE );
    RwD3D1XRaster* d3dRaster = GetD3D1XRaster( raster );
    CD3D1XEnumParser::ConvertRasterFormat( raster, flags );
    //if (raster->cType == rwRASTERTYPETEXTURE || raster->cType == rwRASTERTYPENORMAL)
    //	d3dRaster->resourse = new CD3D1XTexture(raster, (flags&rwRASTERFORMATMIPMAP) != 0, (flags&rwRASTERFORMATPAL8) != 0);
    return raster;
}
