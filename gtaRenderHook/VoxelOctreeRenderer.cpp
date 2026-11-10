// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "VoxelOctreeRenderer.h"
#include "D3DRenderer.h"
#include "RwD3D1XEngine.h"
#include "D3D1XTexture.h"
#include "LightManager.h"
#include "D3D1XStateManager.h"
#include <game_sa\CScene.h>

//CBVoxel CVoxelOctreeRenderer::cb_voxel;
CD3D1XConstantBuffer<CBVoxel>* CVoxelOctreeRenderer::m_pVoxelCB;
RwCamera* CVoxelOctreeRenderer::m_pVoxelCamera = nullptr;
RwRaster* CVoxelOctreeRenderer::voxelRadiance[CVoxelOctreeRenderer::voxelLodCount];
RwRaster* CVoxelOctreeRenderer::voxelClipMap[CVoxelOctreeRenderer::voxelLodCount];
CD3D1XComputeShader* CVoxelOctreeRenderer::m_voxelCS = nullptr;
CD3D1XComputeShader* CVoxelOctreeRenderer::m_radianceInjectCS = nullptr;
void CVoxelOctreeRenderer::Init()
{
    // A second run would otherwise overwrite the handles the first one owns and leak them. Shutdown releases whatever
    // exists and clears every handle it releases, so running it first makes a repeat run start from empty, and it is
    // also the call that releases whatever a run which stopped part way through had already created.
    Shutdown();

    m_voxelCS = new CD3D1XComputeShader( "shaders/VoxelStuffCS.hlsl", "FillVolume" );
    m_radianceInjectCS = new CD3D1XComputeShader( "shaders/VoxelStuffCS.hlsl", "InjectRadiance" );
    m_pVoxelCamera = RwCameraCreate();
    // Every call below reads the camera, so a creation that failed ends the initialisation instead of being
    // dereferenced.
    if ( m_pVoxelCamera == nullptr )
        return;
    RwCameraSetProjection( m_pVoxelCamera, rwPARALLEL );

    RwCameraSetNearClipPlane( m_pVoxelCamera, -200 );
    RwCameraSetFarClipPlane( m_pVoxelCamera, 400 );

    //m_pShadowCamera->projectionType = rwPARALLEL;

    // Init all voxel textures
    for ( int i = 0; i < voxelLodCount; i++ )
    {
        voxelRadiance[i] = RwRasterCreate( voxelTreeSize, voxelTreeSize, voxelTreeSize, rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT888 );
        voxelClipMap[i] = RwRasterCreate( voxelTreeSize, voxelTreeSize, voxelTreeSize, rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT888 );
        // A level that was never created would be installed as the camera's colour buffer below and cleared through on
        // every pass, so a level that failed ends the start-up here. The constant buffer is created after this loop, so
        // every pass that reads one also stops on its own.
        if ( voxelRadiance[i] == nullptr || voxelClipMap[i] == nullptr )
            return;
    }

    // The depth buffer is installed on the camera and cleared through as well, so the same applies to it.
    RwRaster *zRaster = RwRasterCreate( voxelTreeSize, voxelTreeSize, 32, rwRASTERTYPEZBUFFER );
    if ( zRaster == nullptr )
        return;
    RwCameraSetRaster( m_pVoxelCamera, voxelClipMap[0] );
    RwCameraSetZRaster( m_pVoxelCamera, zRaster );

    // The constant buffer has to exist before anything writes through it; SetVoxelLODSize
    // updates it directly.
    m_pVoxelCB = new CD3D1XConstantBuffer<CBVoxel>();
    SetVoxelLODSize( 1 );
    //CameraSize(m_pShadowCamera, 0, tan(3.14f / 2), 1.0f);
    // The camera is positioned and drawn through this frame from the moment it joins the world, and a camera that joins
    // the world without one is read through an empty frame by every pass that walks the world, so a frame that was never
    // created ends the start-up like a raster that was never created. The release below runs before the camera joins the
    // world, so the teardown has no world entry to leave behind, and it leaves the camera empty, which is what the two
    // entry points that read the camera check before they touch a raster.
    RwFrame *frame = RwFrameCreate();
    if ( frame == nullptr )
    {
        Shutdown();
        return;
    }
    RwObjectHasFrameSetFrame( m_pVoxelCamera, frame );
    RpWorldAddCamera( Scene.m_pRpWorld, m_pVoxelCamera );
}



void CVoxelOctreeRenderer::Shutdown()
{
    delete m_pVoxelCB;
    m_pVoxelCB = nullptr;
    for ( int i = 0; i < voxelLodCount; i++ )
    {
        // An empty slot is the normal state before a first start-up and after a previous shutdown, and the release
        // call is a game routine that nothing here establishes a null case for, so only a filled slot reaches it.
        if ( voxelRadiance[i] )
            RwRasterDestroy( voxelRadiance[i] );
        if ( voxelClipMap[i] )
            RwRasterDestroy( voxelClipMap[i] );
        // A released raster left in the array would be released a second time by a repeated shutdown, so each slot
        // is cleared as it is released.
        voxelRadiance[i] = nullptr;
        voxelClipMap[i] = nullptr;
    }
    // The camera only exists once start-up got that far, and the calls below read through it.
    if ( m_pVoxelCamera )
    {
        if ( m_pVoxelCamera->zBuffer )
            RwRasterDestroy( m_pVoxelCamera->zBuffer );
        RwCameraDestroy( m_pVoxelCamera );
        m_pVoxelCamera = nullptr;
    }
    delete m_voxelCS;
    m_voxelCS = nullptr;
    delete m_radianceInjectCS;
    m_radianceInjectCS = nullptr;
}

void CVoxelOctreeRenderer::CleanVoxelOctree()
{
    // Initialisation creates the compute shaders before it can fail, and a compile that failed leaves the object in
    // place with no shader inside it, so the guard covers both a missing object and an object that never became
    // usable: a pass that runs without a usable one has nothing to dispatch through.
    if ( m_voxelCS == nullptr || !m_voxelCS->IsValid() )
        return;
    m_voxelCS->Set();

    auto context = GET_D3D_CONTEXT;
    for ( int i = 0; i < voxelLodCount; i++ )
    {
        //auto uav = GetD3D1XRaster(voxelClipMap[i])->resourse->GetUAV();
        //context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch( voxelTreeSize / 4, voxelTreeSize / 4, voxelTreeSize / 4 );
    }

    m_voxelCS->ReSet();

    ID3D11UnorderedAccessView* uavs[] = { nullptr };
    context->CSSetUnorderedAccessViews( 0, 1, uavs, nullptr );
}

void CVoxelOctreeRenderer::InjectRadiance( RwRaster * shadow, void( *emmissiveObjRender )( ), int voxelTextureID )
{
    // voxelTextureID names the clip map read below and is the level the caller keeps its radiance volume in step
    // with, so it has to name a level that exists.
    if ( voxelTextureID < 0 || voxelTextureID >= voxelLodCount )
        return;
    // The same reason as CleanVoxelOctree: the dispatch below has no shader to run without a usable one.
    if ( m_radianceInjectCS == nullptr || !m_radianceInjectCS->IsValid() )
        return;
    // The constant buffer setter below reads the buffer it is handed, and the light manager's buffer is empty until
    // that manager creates one, so a pass that runs before then has no buffer to hand over. The handle is kept so the
    // value this guard accepts is the value handed on, rather than a second read of the same static.
    CD3D1XConstantBuffer<LightsCB>* lightBuffer = CLightManager::GetBuffer();
    if ( lightBuffer == nullptr )
        return;

    auto context = GET_D3D_CONTEXT;
    m_radianceInjectCS->Set();

    g_pStateMgr->SetRasterCS( voxelClipMap[voxelTextureID], 0 );
    g_pStateMgr->SetRasterCS( shadow, 1 );
    g_pStateMgr->SetConstantBufferCS( lightBuffer, 10 );

    //auto uav = GetD3D1XRaster(voxelRadiance[voxelTextureID])->resourse->GetUAV();
    //context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    context->Dispatch( voxelTreeSize / 4, voxelTreeSize / 4, voxelTreeSize / 4 );

    m_radianceInjectCS->ReSet();

    ID3D11UnorderedAccessView* uavs[] = { nullptr };
    context->CSSetUnorderedAccessViews( 0, 1, uavs, nullptr );
    g_pStateMgr->SetRasterCS( nullptr, 0 );
    g_pStateMgr->SetRasterCS( nullptr, 1 );
}
void CVoxelOctreeRenderer::FilterVoxelOctree()
{
    //auto context = ((CRwD3D1XEngine*)g_pRwCustomEngine)->getRenderer()->getContext();
    //context->GenerateMips(GetD3D1XRaster(voxelRadiance)->resourse->getSRV());
    //context->GenerateMips(GetD3D1XRaster(voxelNormals)->resourse->getSRV());
}
//C886D4     AmbientLightColourForFrame RwRGBAReal
#define AmbientLightColourForFrame (*(RwRGBAReal*)0xC886D4)
#define DirectionalLightColourForFrame (*(RwRGBAReal*)0xC886B4)

// Voxelizes scene using render method to choosen level of detail volume.
void CVoxelOctreeRenderer::RenderToVoxelOctree( void( *render )( ), const int &lod )
{
    // lod selects the clip map this pass renders into, so a value outside the levels that exist would index past the
    // array. Checked here rather than left to the caller, because the index below is derived from it.
    if ( lod < 1 || lod > voxelLodCount )
        return;
    // Start-up can stop before the camera, the constant buffer or this level's colour buffer exist, and all three are
    // written through or cleared through below.
    if ( m_pVoxelCamera == nullptr || m_pVoxelCB == nullptr || voxelClipMap[lod - 1] == nullptr )
        return;

    CD3DRenderer* renderer = ( (CRwD3D1XEngine*)g_pRwCustomEngine )->getRenderer();
    ID3D11DeviceContext* context = renderer->getContext();

    // Sets global voxel properties.
    m_pVoxelCB->Update();
    auto buff = m_pVoxelCB->getBuffer();
    context->GSSetConstantBuffers( 4, 1, &buff );
    //context->VSSetConstantBuffers(4, 1, &m_pVoxelCB);
    g_pStateMgr->SetConstantBufferPS( m_pVoxelCB, 4 );
    g_pStateMgr->SetConstantBufferCS( m_pVoxelCB, 4 );

    RwCameraSetRaster( m_pVoxelCamera, voxelClipMap[lod - 1] );

    RwCameraClear( m_pVoxelCamera, gColourTop, rwCAMERACLEARZ );

    // The pass below draws through the camera, and the matching end call belongs only to an update that started, so
    // the annotation scope opens after this check rather than before it.
    if ( RwCameraBeginUpdate( m_pVoxelCamera ) == nullptr )
        return;

    renderer->BeginDebugEvent( L"Voxel Buffer" );
    // Captured and put back, so a caller that entered this pass with the depth test off still has it off afterwards.
    const BOOL oldDepthEnable = g_pStateMgr->GetDepthEnable();
    g_pRwCustomEngine->RenderStateSet( rwRENDERSTATEZTESTENABLE, 0 );
    render();
    g_pStateMgr->SetDepthEnable( oldDepthEnable );
    RwCameraEndUpdate( m_pVoxelCamera );

    ID3D11UnorderedAccessView* uavs = nullptr;
    context->OMSetRenderTargetsAndUnorderedAccessViews( 0, { nullptr }, nullptr, 3, 1, &uavs, nullptr );

    renderer->EndDebugEvent();
}

void CVoxelOctreeRenderer::SetVoxelTex( RwRGBA &color )
{
    RwRGBA rgba;
    RwRGBAFromRwRGBAReal( &rgba, &DirectionalLightColourForFrame );

    g_pRwCustomEngine->RenderStateSet( RwRenderState::rwRENDERSTATEBORDERCOLOR, RWRGBALONG( 0, 0, 0, 0 ) );
    g_pRwCustomEngine->RenderStateSet( RwRenderState::rwRENDERSTATETEXTUREADDRESS, rwTEXTUREADDRESSBORDER );
    g_pRwCustomEngine->RenderStateSet( RwRenderState::rwRENDERSTATETEXTUREFILTER, rwFILTERLINEAR );

    /*ID3D11ShaderResourceView* srv[] = { GetD3D1XRaster(voxelRadiance[0])->resourse->GetSRV(),
                                        GetD3D1XRaster(voxelRadiance[1])->resourse->GetSRV(),
                                        GetD3D1XRaster(voxelRadiance[2])->resourse->GetSRV(), };
    GET_D3D_CONTEXT->PSSetShaderResources(6, 3, srv);*/
}

void CVoxelOctreeRenderer::SetVoxelLODSize( const int & lod )
{
    // Same levels as RenderToVoxelOctree. The guard also keeps the shift below well defined: its exponent is the level
    // count minus lod, which is only representable while lod names a level.
    if ( lod < 1 || lod > voxelLodCount )
        return;
    // Both members below are written through, and initialisation can stop before either exists.
    if ( m_pVoxelCB == nullptr || m_pVoxelCamera == nullptr )
        return;

    RwV2d vw;
    vw.x = vw.y = (float)( ( voxelTreeSize / 2 ) / ( 1 << ( voxelLodCount - lod ) ) );
    m_pVoxelCB->data.voxelGridScale = (float)( 1 << ( voxelLodCount - lod ) );
    m_pVoxelCB->Update();
    RwCameraSetViewWindow( m_pVoxelCamera, &vw );
}
