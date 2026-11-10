// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "CubemapReflectionRenderer.h"
#include "D3D1XDepthStencilTexture.h"
#include "D3D1XRenderBuffersManager.h"
#include "D3D1XStateManager.h"
#include "D3DRenderer.h"
#include "RwD3D1XEngine.h"
#include "RwMethods.h"
#include "stdafx.h"
#include <game_sa\CScene.h>
#define _USE_MATH_DEFINES
#include <math.h>

#define ENVMAPSIZE 512
#define MIPLEVELS 7

CCubemapReflectionRenderer::CCubemapReflectionRenderer( int size )
    : m_nCubemapSize( size )
{
    ID3D11Device *device = GET_D3D_DEVICE;

    // The mip count below is derived from the size, and log2 is undefined at zero while one mip less than a size of
    // one would wrap, so a cubemap too small to carry any mip level is refused before anything is created.
    if ( m_nCubemapSize < 2 )
        return;

    m_pReflCamera = RwCameraCreate();
    // Every call below reads the camera, so a creation that failed ends the constructor instead of being dereferenced.
    if ( m_pReflCamera == nullptr )
        return;
    RwCameraSetProjection( m_pReflCamera, RwCameraProjection::rwPERSPECTIVE );

    RwCameraSetNearClipPlane( m_pReflCamera, 0.1f );
    RwCameraSetFarClipPlane( m_pReflCamera, 100 );

    RwRaster *frameRaster =
        RwRasterCreate( m_nCubemapSize, m_nCubemapSize, 32, rwRASTERTYPECAMERATEXTURE );
    RwRaster *zRaster =
        RwRasterCreate( m_nCubemapSize, m_nCubemapSize, 32, rwRASTERTYPEZBUFFER );
    // The pass clears and renders through both rasters, so a camera that came back without them ends the constructor
    // here. Whichever raster was created is released, because the camera has not taken ownership of either yet.
    if ( frameRaster == nullptr || zRaster == nullptr )
    {
        // Exactly one of the two can be the one that failed, so each release is made conditional rather than left to
        // the release call to accept an empty raster.
        if ( frameRaster )
            RwRasterDestroy( frameRaster );
        if ( zRaster )
            RwRasterDestroy( zRaster );
        return;
    }
    RwCameraSetRaster( m_pReflCamera, frameRaster );
    RwCameraSetZRaster( m_pReflCamera, zRaster );

    CameraSize( m_pReflCamera, nullptr, tanf( 3.14f / 4 ), 1.0f );
    m_pReflCameraFrame = RwFrameCreate();
    // The pass orients and moves the face camera through this frame, so a frame that was never created ends the
    // constructor as well rather than leaving the camera attached to nothing.
    if ( m_pReflCameraFrame == nullptr )
        return;
    RwObjectHasFrameSetFrame( m_pReflCamera, m_pReflCameraFrame );
    RpWorldAddCamera( Scene.m_pRpWorld, m_pReflCamera );
    UINT mipCount = max( (UINT)log2( m_nCubemapSize ) - 1, 1 );
    // Create cubic depth stencil texture.
    D3D11_TEXTURE2D_DESC dstex{};
    dstex.Width              = m_nCubemapSize;
    dstex.Height             = m_nCubemapSize;
    dstex.ArraySize          = cubemapFaceCount;
    dstex.SampleDesc.Count   = 1;
    dstex.SampleDesc.Quality = 0;
    dstex.Usage              = D3D11_USAGE_DEFAULT;
    dstex.CPUAccessFlags     = 0;

    // Create the cube map for env map render target
    dstex.Format    = DXGI_FORMAT_R8G8B8A8_UNORM;
    dstex.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    dstex.MiscFlags =
        D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_TEXTURECUBE;
    // The texture, the render target views and the shader resource view below all describe this same mip chain, so
    // the level count is written once and nothing overwrites it after this point.
    dstex.MipLevels = mipCount;
    if ( FAILED( device->CreateTexture2D( &dstex, NULL, &g_pEnvMap ) ) )
    {
        g_pDebug->printMsg( "Failed to create depth env map.", 0 );
        return;
    }

    // Create the all-face render target view
    D3D11_RENDER_TARGET_VIEW_DESC DescRT{};
    DescRT.Format                         = dstex.Format;
    DescRT.ViewDimension                  = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    DescRT.Texture2DArray.FirstArraySlice = 0;
    DescRT.Texture2DArray.ArraySize       = cubemapFaceCount;
    DescRT.Texture2DArray.MipSlice        = 0;
    if ( FAILED( device->CreateRenderTargetView( g_pEnvMap, &DescRT,
                                                 &g_pEnvMapRTV ) ) )
    {
        g_pDebug->printMsg( "Failed to create depth env map.", 0 );
        return;
    }
    // Create the one-face render target views
    DescRT.Texture2DArray.ArraySize = 1;
    for ( int i = 0; i < cubemapFaceCount; ++i )
    {
        DescRT.Texture2DArray.FirstArraySlice = i;
        if ( FAILED( device->CreateRenderTargetView( g_pEnvMap, &DescRT,
                                                     &g_apEnvMapOneRTV[i] ) ) )
        {
            g_pDebug->printMsg( "Failed to create depth env map.", 0 );
            return;
        }
    }

    // Create the shader resource view for the cubic env map
    D3D11_SHADER_RESOURCE_VIEW_DESC SRVDesc{};
    ZeroMemory( &SRVDesc, sizeof( SRVDesc ) );
    SRVDesc.Format                      = dstex.Format;
    SRVDesc.ViewDimension               = D3D11_SRV_DIMENSION_TEXTURECUBE;
    SRVDesc.TextureCube.MipLevels       = mipCount;
    SRVDesc.TextureCube.MostDetailedMip = 0;
    if ( FAILED( device->CreateShaderResourceView( g_pEnvMap, &SRVDesc,
                                                   &g_pEnvMapSRV ) ) )
    {
        g_pDebug->printMsg( "Failed to create depth env map.", 0 );
        return;
    }
    // Written last, once every view it stands for exists, so the pass can never observe a ready cubemap whose views
    // are still being created.
    m_bResourcesReady = true;
}

CCubemapReflectionRenderer::~CCubemapReflectionRenderer()
{
    // Every access below reads through the camera handle, and the constructor can end before that handle exists, so a
    // camera that was never created is the one case where this teardown has nothing to undo.
    if ( m_pReflCamera )
    {
        // A constructor that stopped before both rasters were installed leaves the two fields empty, and the release
        // call is a game routine with no null case established here, so each raster is released only when present.
        if ( m_pReflCamera->zBuffer )
            RwRasterDestroy( m_pReflCamera->zBuffer );
        if ( m_pReflCamera->frameBuffer )
            RwRasterDestroy( m_pReflCamera->frameBuffer );
        RwCameraDestroy( m_pReflCamera );
    }
    if ( m_pReflCB )
        m_pReflCB->Release();
    if ( g_pEnvMap )
        g_pEnvMap->Release();
    if ( g_pEnvMapRTV )
        g_pEnvMapRTV->Release();
    for ( int i = 0; i < cubemapFaceCount; ++i )
    {
        if ( g_apEnvMapOneRTV[i] )
            g_apEnvMapOneRTV[i]->Release();
    }
    if ( g_pEnvMapSRV )
        g_pEnvMapSRV->Release();
}

#define FindPlayerCoors( outPoint, playerIndex )                               \
    ( (RwV3d * (__cdecl *)(RwV3d *, int))0x56E010 )( outPoint, playerIndex )
void CCubemapReflectionRenderer::RenderToCubemap( void ( *renderCB )() )
{
    // The pass draws into the face views and then generates the mip chain from the resource view, so without a
    // complete cubemap there is nothing here to produce. Refused before the cull mode is captured below, so no
    // capture is left without its restore.
    if ( !m_bResourcesReady )
        return;
    // The render callback turns culling off so all six faces keep their geometry, so the mode in force when the
    // pass started has to come back once the last face is done. Restoring inside the callback would cover only
    // the first face, because the callback is invoked once per face.
    const RwCullMode savedCullMode = g_pStateMgr->GetCullMode();
    // Move reflection camera to game camera position.
    auto  s_cam_frame = RwCameraGetFrame( m_pReflCamera );
    RwV3d campos;
    // campos =
    // *RwMatrixGetPos(RwFrameGetMatrix(RwCameraGetFrame(Scene.m_pRwCamera)));
    FindPlayerCoors( &campos, 0 );
    // Render each face.
    RwV3d At, Up, Right;
    At    = {1.0f, 0.0f, 0.0f};
    Up    = {0.0f, 1.0f, 0.0f};
    Right = {0.0f, 0.0f, 1.0f};

    RenderOneFace( renderCB, 0, 0, At, 270, Up, campos );   // forward
    RenderOneFace( renderCB, 1, 0, Right, 90, Up, campos ); // backward
    RenderOneFace( renderCB, 2, 270, At, 0, Up, campos );   // right
    RenderOneFace( renderCB, 3, 90, At, 0, Up, campos );    // left
    RenderOneFace( renderCB, 4, 0, At, 0, Up, campos );     // down
    RenderOneFace( renderCB, 5, 0, At, 180, Up, campos );   // up
    ID3D11DeviceContext *context = GET_D3D_CONTEXT;
    context->GenerateMips( g_pEnvMapSRV );
    g_pStateMgr->SetCullMode( savedCullMode );
}

void CCubemapReflectionRenderer::RenderOneFace(
    void ( *renderCB )(),
    int id, float angleA, RwV3d axisA, float angleB, RwV3d axisB, RwV3d camPos /*const RW::V3d&At, const RW::V3d&Up, const RW::V3d&Right, RW::V3d&  Pos*/ )
{
    // Public entry point, so the checks the pass makes are repeated here rather than assumed: a face id outside the
    // views that exist would read past the array, and an incomplete cubemap has no face to draw into.
    if ( !m_bResourcesReady )
        return;
    if ( id < 0 || id >= cubemapFaceCount )
        return;

    ID3D11DeviceContext *context = GET_D3D_CONTEXT;

    RwFrameSetIdentity( m_pReflCameraFrame );
    RwFrameRotate( m_pReflCameraFrame, &axisA, angleA, rwCOMBINEREPLACE );
    RwFrameRotate( m_pReflCameraFrame, &axisB, angleB, rwCOMBINEPOSTCONCAT );
    RwFrameTranslate( m_pReflCameraFrame, &camPos, rwCOMBINEPOSTCONCAT );
    RwFrameUpdateObjects( m_pReflCameraFrame );
    RwCameraSetNearClipPlane( m_pReflCamera, 0.1f );
    RwCameraSetFarClipPlane( m_pReflCamera, 100.0f );
    RwCameraClear( m_pReflCamera, gColourTop, rwCAMERACLEARZ );

    // The clear and the draw below both assume the face camera is ready to receive them, and the matching end call
    // belongs only to an update that started.
    if ( RwCameraBeginUpdate( m_pReflCamera ) == nullptr )
        return;

    float ClearColor[4] = {0.0f, 0.0f, 0.0f, 0.4f};
    context->ClearRenderTargetView( g_apEnvMapOneRTV[id], ClearColor );
    // GetD3D1XRaster only offsets the raster pointer it is given, so a camera without a depth raster would hand back
    // a small non-null address and the field read below would fault. The raster is checked before it is offset.
    const RwD3D1XRaster *depthRaster =
        m_pReflCamera->zBuffer ? GetD3D1XRaster( m_pReflCamera->zBuffer ) : nullptr;
    auto depthBuffer = depthRaster ? dynamic_cast<CD3D1XDepthStencilTexture *>( depthRaster->resourse ) : nullptr;
    ID3D11DepthStencilView *depthView = depthBuffer ? depthBuffer->GetDepthStencilView() : nullptr;
    ID3D11RenderTargetView *aRTViews[1] = {g_apEnvMapOneRTV[id]};
    context->OMSetRenderTargets( 1, aRTViews, depthView );
    // FlushStates can re-issue the target set from the state cache, and the direct bind above leaves that cache holding
    // whatever the previous pass bound, so the face is recorded as well: the flush then re-issues the face instead of
    // replacing it with the previous pass's targets.
    g_pStateMgr->SetRenderTargets( 1, aRTViews, depthView );
    D3D11_VIEWPORT vp{};
    vp.Width    = (FLOAT)m_nCubemapSize;
    vp.Height   = (FLOAT)m_nCubemapSize;
    vp.MaxDepth = 1.0;
    g_pStateMgr->SetViewport( vp );
    g_pStateMgr->FlushStates();
    renderCB();
    RwCameraEndUpdate( m_pReflCamera );
}

void CCubemapReflectionRenderer::SetCubemap()
{
    ID3D11DeviceContext *context = GET_D3D_CONTEXT;
    // An unusable cubemap is bound as an explicit null rather than skipped, so the shaders that sample this slot read
    // zero instead of whatever an earlier pass happened to leave there.
    ID3D11ShaderResourceView *envMapSRV = m_bResourcesReady ? g_pEnvMapSRV : nullptr;
    context->PSSetShaderResources( 5, 1, &envMapSRV );
}
