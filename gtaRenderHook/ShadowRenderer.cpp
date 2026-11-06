// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "ShadowRenderer.h"
#include "D3DRenderer.h"
#include "RwD3D1XEngine.h"
#include "D3D1XTexture.h"
#include "D3D1XStateManager.h"
#include "SettingsHolder.h"
#include "RwMethods.h"
#include "D3D1XBuffer.h"
#include <game_sa\CScene.h>
#include <game_sa\CCamera.h>
#include "DeferredRenderer.h"
#include "D3D1XRenderBuffersManager.h"
#include "D3D1XShader.h"
#include "FullscreenQuad.h"
RW::BBox	CShadowRenderer::m_LightBBox[kMaxShadowCascades];
RwV3d	CShadowRenderer::m_LightPos[kMaxShadowCascades];
RW::Matrix	CShadowRenderer::m_LightSpaceMatrix[kMaxShadowCascades];
RW::Matrix	CShadowRenderer::m_InvLightSpaceMatrix[kMaxShadowCascades];
RW::V3d CShadowRenderer::m_vFrustumCorners[kMaxShadowCascades][8];
ShadowSettingsBlock gShadowSettings;

// The shaders declare their own shadow constants with four cascade slots, so the bound in
// ShadowRenderer.h cannot move without the shaders moving with it.
static_assert( sizeof( CBShadows::ViewProj ) / sizeof( CBShadows::ViewProj[0] ) == 4,
               "shadow cbuffer layout and the shaders assume four cascades" );
static_assert( sizeof( CBShadows::FadeDistances ) / sizeof( float ) == kMaxShadowCascades + 1,
               "fade distances need one boundary per cascade plus the far edge" );
static_assert( sizeof( CBShadows::ShadowBias ) / sizeof( float ) == kMaxShadowCascades,
               "shadow bias entries must match the cascade count" );
static_assert( sizeof( ShadowSettingsBlock::BiasCoefficients ) / sizeof( float ) == kMaxShadowCascades,
               "one bias coefficient per cascade, indexed by cascade ID minus one" );
static_assert( sizeof( ShadowSettingsBlock::DistanceCoefficients ) / sizeof( float ) == kMaxShadowCascades - 1,
               "every cascade after the first has a start distance multiplier" );

CShadowRenderer::CShadowRenderer()
{
    RwV2d vw;
    m_pShadowCamera = RwCameraCreate();
    RwCameraSetProjection( m_pShadowCamera, rwPARALLEL );
    RwCameraSetNearClipPlane( m_pShadowCamera, -150 );
    RwCameraSetFarClipPlane( m_pShadowCamera, 1500 );
    RwCameraSetRaster( m_pShadowCamera, nullptr );
    int maxTexSize = 0;
    // The engine reports zero for a feature level it does not list, and treating that as a bound would ask for a
    // zero wide atlas whose raster nothing can dereference. Same floor as Load and the size callback apply.
    if ( g_pRwCustomEngine->GetMaxTextureSize( maxTexSize ) && maxTexSize >= 16 )
        gShadowSettings.Size = (UINT)min( (int)gShadowSettings.Size, maxTexSize / gShadowSettings.ShadowCascadeCount );
    RwCameraSetZRaster( m_pShadowCamera, RwRasterCreate( gShadowSettings.Size * gShadowSettings.ShadowCascadeCount, gShadowSettings.Size, 32, rwRASTERTYPEZBUFFER | rwRASTERFORMAT32 ) );
    vw.x = vw.y = 40;
    RwCameraSetViewWindow( m_pShadowCamera, &vw );
    gDebugSettings.DebugRenderTargetList.push_back( m_pShadowCamera->zBuffer );
    RwObjectHasFrameSetFrame( m_pShadowCamera, RwFrameCreate() );
    RpWorldAddCamera( Scene.m_pRpWorld, m_pShadowCamera );
    m_pLightViewProj = nullptr;
    // The depth clear samples nothing, so it must not be BlitPS: that entry point reads txPrevFrame, which the
    // depth-only pass leaves unbound.
    m_pDepthClearPS = new CD3D1XPixelShader( "shaders/Quad.hlsl", "DepthClearPS" );

    m_pLightCB = new CD3D1XConstantBuffer<CBShadows>();
    m_pLightCB->SetDebugName( "ShadowsCB" );
    for ( int i = 0; i < kMaxShadowCascades + 1; i++ )
        m_fShadowDistances[i] = 0.0f;
}


CShadowRenderer::~CShadowRenderer()
{
    delete m_pLightCB;
    delete m_pDepthClearPS;
    RwRasterDestroy( m_pShadowCamera->zBuffer );
    RwCameraDestroy( m_pShadowCamera );
}
// Transforms light camera to fit needed camera frustum and look in needed direction.
void CShadowRenderer::DirectionalLightTransform( RwCamera* mainCam, const RW::V3d & lightDir, int shadowCascade )
{
    m_LightPos[shadowCascade] = CalculateCameraPos( mainCam, lightDir, shadowCascade ).getRWVector();
}

RW::V3d CShadowRenderer::CalculateCameraPos( RwCamera* mainCam, const RW::V3d & lightDir, int shadowCascade )
{
    RW::V3d vLightPos, vLightDir, vFrustrumCenter;
    RW::V3d vLightBasis[3];

    vFrustrumCenter = RW::V3d{ 0, 0, 0 };

    vLightBasis[2] = -lightDir;
    vLightBasis[2].normalize();
    vLightBasis[1] = { 0, 1, 0 };
    // The cross product collapses when the light direction runs along the reference up vector,
    // so fall back to another reference axis to keep the basis orthonormal instead of NaN.
    // The guard already proves the light direction lies within about 0.81 degrees of the Y axis, so
    // the X axis is at most 0.0142 away from it and is always a safe fallback.
    if ( fabsf( vLightBasis[1].dot( vLightBasis[2] ) ) > 0.9999f )
        vLightBasis[1] = RW::V3d{ 1, 0, 0 };
    vLightBasis[0] = vLightBasis[1].cross( vLightBasis[2] );
    vLightBasis[0].normalize();
    vLightBasis[1] = vLightBasis[0].cross( vLightBasis[2] );
    vLightBasis[1].normalize();

    m_LightSpaceMatrix[shadowCascade] = { { vLightBasis[0] }, { vLightBasis[1] }, { vLightBasis[2] }, {} };
    m_InvLightSpaceMatrix[shadowCascade] = m_LightSpaceMatrix[shadowCascade].inverse();

    //auto oldFP = mainCam->farPlane;
    //auto oldNP = mainCam->nearPlane;
    /*RwCameraSetNearClipPlane(mainCam, m_fShadowDistances[shadowCascade]);
    RwCameraSetFarClipPlane(mainCam, m_fShadowDistances[shadowCascade + 1]);

    RwCameraSync(mainCam);*/

    const RwReal fNear = m_fShadowDistances[shadowCascade], fFar = m_fShadowDistances[shadowCascade + 1];
    CalculateFrustumPoints( fNear, fFar, mainCam, m_vFrustumCorners[shadowCascade] );

    // Stable cascade: enclose the frustum slice in its smallest bounding sphere. Its radius depends only on the slice
    // distances and the field of view, so the cascade keeps its size while the camera turns (a box fitted to the corners
    // grows and shrinks, which makes every shadow edge crawl). The sphere's centre lies on the view axis where the near
    // and far corners are equally far away.
    const RwReal halfDiag = sqrtf( mainCam->viewWindow.x * mainCam->viewWindow.x + mainCam->viewWindow.y * mainCam->viewWindow.y );
    const RwReal nearRadius = halfDiag * fNear, farRadius = halfDiag * fFar;
    RwReal centerDist = fNear;
    // A slice whose near and far distances coincide would divide by zero here.
    if ( fFar - fNear > 1e-4f )
        centerDist = ( fFar * fFar + farRadius * farRadius - fNear * fNear - nearRadius * nearRadius ) / ( 2.0f * ( fFar - fNear ) );
    centerDist = min( max( centerDist, fNear ), fFar );
    RwReal radius = max( sqrtf( ( centerDist - fNear ) * ( centerDist - fNear ) + nearRadius * nearRadius ),
                         sqrtf( ( fFar - centerDist ) * ( fFar - centerDist ) + farRadius * farRadius ) );
    radius = ceilf( radius * 16.0f ) / 16.0f; // no float noise in the cascade size

    RwMatrix *camMatrix = RwFrameGetMatrix( RwCameraGetFrame( mainCam ) );
    RW::V3d camPos = *RwMatrixGetPos( camMatrix );
    RW::V3d camAt = *RwMatrixGetAt( camMatrix );
    RW::V3d center = camPos + camAt * centerDist;

    // Move the centre in whole shadow-map texels (in light space), so the texel grid stays fixed in the world and static
    // shadows don't shimmer when the camera moves.
    const RwReal texelSize = 2.0f * radius / static_cast<RwReal>( gShadowSettings.Size );
    RW::V3d lightCenter = center * m_InvLightSpaceMatrix[shadowCascade];
    lightCenter = { floorf( lightCenter.getX() / texelSize ) * texelSize, floorf( lightCenter.getY() / texelSize ) * texelSize,
                    lightCenter.getZ() };
    m_LightBBox[shadowCascade] = { lightCenter - RW::V3d{ radius, radius, radius }, lightCenter + RW::V3d{ radius, radius, radius } };
    vFrustrumCenter = lightCenter * m_LightSpaceMatrix[shadowCascade];
    /*RwCameraSetNearClipPlane(mainCam, oldNP);
    RwCameraSetFarClipPlane(mainCam, oldFP);
    RwCameraSync(mainCam);*/

    return vFrustrumCenter;
}
void CShadowRenderer::CalculateFrustumPoints( RwReal fNear, RwReal fFar, RwCamera * camera, RW::V3d * points )
{
    RwFrame*	camFrame = RwCameraGetFrame( camera );
    RwMatrix*	camMatrix = RwFrameGetMatrix( camFrame );
    RW::V3d		camPos = *RwMatrixGetPos( camMatrix );
    RW::V3d		camAt = *RwMatrixGetAt( camMatrix );
    RW::V3d		camRight = *RwMatrixGetRight( camMatrix );
    RW::V3d		camUp = *RwMatrixGetUp( camMatrix );

    RW::V3d nearCenter = camPos + camAt * fNear;
    RW::V3d farCenter = camPos + camAt * fFar;

    RwReal nearHeight = 2 * camera->viewWindow.y * fNear;
    RwReal farHeight = 2 * camera->viewWindow.y * fFar;
    RwReal nearWidth = 2 * camera->viewWindow.x * fNear;
    RwReal farWidth = 2 * camera->viewWindow.x * fFar;

    RW::V3d nearBottom = nearCenter - camUp * nearHeight*0.5f;
    RW::V3d nearTop = nearCenter + camUp * nearHeight*0.5f;

    RW::V3d farBottom = farCenter - camUp * farHeight*0.5f;
    RW::V3d farTop = farCenter + camUp * farHeight*0.5f;

    RW::V3d farRightOffset = camRight * ( farWidth*0.5f );
    RW::V3d nearRightOffset = camRight * ( nearWidth*0.5f );

    // near plane
    points[0] = nearBottom - nearRightOffset;
    points[1] = nearBottom + nearRightOffset;
    points[2] = nearTop - nearRightOffset;
    points[3] = nearTop + nearRightOffset;

    // far plane
    points[4] = farBottom - farRightOffset;
    points[5] = farBottom + farRightOffset;
    points[6] = farTop - farRightOffset;
    points[7] = farTop + farRightOffset;

}

void CShadowRenderer::SelectCascadesToUpdate( const RW::V3d &lightDir )
{
    const UINT interval = max( gShadowSettings.GetUInt( "FarCascadeUpdateInterval" ), 1u );
    RW::V3d dir = lightDir;
    dir.normalize();
    // the sun moves slowly; a jump means a switch between sun and moon, or a teleport of the time of day
    const bool lightJumped = dir.dot( m_vLastLightDir ) < 0.9999f;
    m_vLastLightDir = dir;
    RW::V3d camPos = ( Scene.m_pRwCamera && RwCameraGetFrame( Scene.m_pRwCamera ) )
                         ? RW::V3d( *RwMatrixGetPos( RwFrameGetMatrix( RwCameraGetFrame( Scene.m_pRwCamera ) ) ) )
                         : m_vLastCameraPos;
    RW::V3d camAt  = ( Scene.m_pRwCamera && RwCameraGetFrame( Scene.m_pRwCamera ) )
                         ? RW::V3d( *RwMatrixGetAt( RwFrameGetMatrix( RwCameraGetFrame( Scene.m_pRwCamera ) ) ) )
                         : m_vLastCameraAt;
    if ( camAt.length() > 0.0001f )
        camAt.normalize();
    const bool cameraJumped = ( camPos - m_vLastCameraPos ).length() > 25.0f ||
                              ( m_vLastCameraAt.length() > 0.5f && camAt.dot( m_vLastCameraAt ) < 0.85f );
    m_vLastCameraPos = camPos;
    m_vLastCameraAt  = camAt;
    const bool full = m_bForceFullUpdate || !m_bShadowHistoryValid || lightJumped || cameraJumped || interval == 1;
    m_bForceFullUpdate = false;
    // the first two cascades cover the view up close, where moving objects and their shadows are; further cascades
    // take turns, so a frame never re-renders more than one of them when the interval is at least their count
    for ( int i = 0; i < kMaxShadowCascades; i++ )
        m_bCascadeUpdated[i] = full || i < 2 || ( m_uiFrame + i ) % interval == 0;
    m_uiFrame++;
}

bool CShadowRenderer::AllCascadesUpdated() const
{
    for ( int i = 0; i < gShadowSettings.ShadowCascadeCount; i++ )
        if ( !m_bCascadeUpdated[i] )
            return false;
    return true;
}

// Clears one cascade's region of the shadow atlas (the viewport set for it) to the far plane: a quad at depth 1 with the
// depth test set to always pass. ClearDepthStencilView can only clear the whole atlas.
void CShadowRenderer::ClearCascade( int cascade )
{
    g_pStateMgr->SetDepthEnable( true );
    g_pStateMgr->SetZWriteEnable( true );
    g_pStateMgr->SetDepthFunc( D3D11_COMPARISON_ALWAYS );
    const RwCullMode oldCullMode = g_pStateMgr->GetCullMode();
    const BOOL oldBlendState = g_pStateMgr->GetAlphaBlendEnable();
    g_pStateMgr->SetCullMode( rwCULLMODECULLNONE );
    g_pStateMgr->SetAlphaBlendEnable( FALSE );
    m_pDepthClearPS->Set();
    CFullscreenQuad::Draw();
    g_pStateMgr->SetDepthFunc( D3D11_COMPARISON_LESS_EQUAL );
    g_pStateMgr->SetCullMode( oldCullMode );
    g_pStateMgr->SetAlphaBlendEnable( oldBlendState );
}

void CShadowRenderer::RenderShadowToBuffer( int cascade, void( *render )( int cascade ) )
{
    // kept from an earlier frame: its depth, light matrix and fit stay as they were
    if ( !m_bCascadeUpdated[cascade] )
        return;
    CD3DRenderer* renderer = static_cast<CRwD3D1XEngine*>( g_pRwCustomEngine )->getRenderer();
    renderer->BeginDebugEvent( L"Shadow Buffer" );

    RwFrame*	shadowCamFrame = RwCameraGetFrame( m_pShadowCamera );
    RwMatrix*	shadowCamMatrix = RwFrameGetMatrix( shadowCamFrame );
    //RwV3d lightPos = m_LightPos[cascade];//(m_LightBBox[cascade].getCenter()*m_InvLightSpaceMatrix[cascade]).getRWVector();
    // Transform shadow camera to main camera position. TODO: move it to the center of frustum to cover full scene.
    //RwFrameTranslate(shadowCamFrame, &lightPos, rwCOMBINEREPLACE);

    //RwV3d invCamPos;
    //lightPos = *RwMatrixGetPos(shadowCamMatrix);
    //RwV3dScale(&invCamPos, &lightPos, -1.0f);

    // Move camera to the origin position.
    //RwFrameTranslate(shadowCamFrame, &invCamPos, rwCOMBINEPOSTCONCAT);

    // Rotate camera towards light direction
    shadowCamMatrix->at = m_LightSpaceMatrix[cascade].getAt().getRW3Vector();
    shadowCamMatrix->up = m_LightSpaceMatrix[cascade].getUp().getRW3Vector();
    shadowCamMatrix->right = m_LightSpaceMatrix[cascade].getRight().getRW3Vector();
    shadowCamMatrix->pos = m_LightPos[cascade];
    shadowCamMatrix->flags = 0;
    shadowCamMatrix->pad1 = 0;
    shadowCamMatrix->pad2 = 0;
    shadowCamMatrix->pad3 = 0x3F800000;

    // Move camera back to needed position.
    //RwFrameTranslate(shadowCamFrame, &lightPos, rwCOMBINEPOSTCONCAT);
    float viewSize = max( m_LightBBox[cascade].getSizeX()*0.5f, m_LightBBox[cascade].getSizeY()*0.5f );
    // Set light orthogonal projection parameters.
    RwV2d vw{ viewSize, viewSize };
    RwCameraSetViewWindow( m_pShadowCamera, &vw );
    float fLightZFar = m_LightBBox[cascade].getSizeZ();//faLightDim[1];
    RwCameraSetNearClipPlane( m_pShadowCamera, -500 /*-fLightZFar-25*/ );
    RwCameraSetFarClipPlane( m_pShadowCamera, fLightZFar + kShadowFarClipMargin );
    RwCameraSync( m_pShadowCamera );
    //
    RwCameraBeginUpdate( m_pShadowCamera );
    // Render in one of texture corners, this is done to use less textures.   
    D3D11_VIEWPORT vp;
    vp.Width = static_cast<FLOAT>( gShadowSettings.Size );
    vp.Height = static_cast<FLOAT>( gShadowSettings.Size );
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    vp.TopLeftX = (float)( gShadowSettings.Size * cascade );
    vp.TopLeftY = 0;
    g_pStateMgr->SetViewport( vp );
    if ( !AllCascadesUpdated() )
        ClearCascade( cascade );

    RwGraphicsMatrix* view = (RwGraphicsMatrix *)&RwD3D9D3D9ViewTransform;
    RwGraphicsMatrix* proj = (RwGraphicsMatrix *)&RwD3D9D3D9ProjTransform;
    g_pRenderBuffersMgr->Multipy4x4Matrices( (RwGraphicsMatrix *)&m_pLightCB->data.ViewProj[cascade], view, proj );
    // Clamp instead of clip at the near plane: a tall building far towards a low sun lies in front of it and still casts.
    g_pStateMgr->SetDepthClipEnable( false );
    render( cascade );
    g_pStateMgr->SetDepthClipEnable( true );
    RwCameraEndUpdate( m_pShadowCamera );
    renderer->EndDebugEvent();
}
void CShadowRenderer::SetShadowBuffer() const
{
    // If shadow rendering has not ended we don't need to set shadow buffer
    m_pLightCB->data.ShadowSize = gShadowSettings.Size;
    m_pLightCB->data.CascadeCount = gShadowSettings.ShadowCascadeCount;
    for ( auto i = 0; i < kMaxShadowCascades; i++ )
        m_pLightCB->data.ShadowBias[i] = gShadowSettings.BiasCoefficients[i];
    m_pLightCB->Update();
    g_pStateMgr->SetConstantBufferPS( m_pLightCB, 4 );
    //g_pStateMgr->SetConstantBufferCS(m_pLightCB, 4);
    // Border addressing is what the PCSS blocker search needs: a tap outside the cascade has to read the far
    // plane instead of wrapping to the opposite edge. It is captured here because samLinear is shared with
    // every later pass, which expects whatever mode the surrounding render had set.
    m_eSavedAddressU = g_pStateMgr->GetTextureAdressU();
    m_eSavedAddressV = g_pStateMgr->GetTextureAdressV();
    m_eSavedAddressW = g_pStateMgr->GetTextureAdressW();
    g_pStateMgr->SetTextureAdressUV( rwTEXTUREADDRESSBORDER );
    g_pStateMgr->SetRaster( m_pShadowCamera->zBuffer, 4 );
}

void CShadowRenderer::UnsetShadowBuffer() const
{
    // Slot 4 is declared twice in Deferred.hlsl (txShadow and txLighting), so it must not keep pointing at the
    // shadow atlas once the lighting passes are done: FinalPassPS reads it as the lighting raster.
    g_pStateMgr->SetRaster( nullptr, 4 );
    // SetTextureAdressUV writes U, V and W, so the unbind has to put back all three. W is the coordinate the
    // 3D lookup passes read: samLinear addresses a Texture3D through it, so leaving it on border addressing
    // makes every volume sample outside 0-1 read as zero instead of tiling.
    g_pStateMgr->SetTextureAdressU( m_eSavedAddressU );
    g_pStateMgr->SetTextureAdressV( m_eSavedAddressV );
    g_pStateMgr->SetTextureAdressW( m_eSavedAddressW );
}

void CShadowRenderer::CalculateShadowDistances( const RwReal fNear, const RwReal fFar )
{
    float farDist = min( gShadowSettings.GetFloat( "MaxDrawDistance" ), fFar );
    m_fShadowDistances[0] = fNear;
    /*for (auto i = 0; i < 4; i++)
    {
        m_fShadowDistances[i] = fNear * powf(farDist / fNear, i / (float)gShadowSettings.ShadowCascadeCount);
    }*/
    m_fShadowDistances[1] = fNear + farDist * gShadowSettings.DistanceCoefficients[0];
    m_fShadowDistances[2] = fNear + farDist * gShadowSettings.DistanceCoefficients[1];
    m_fShadowDistances[3] = fNear + farDist * gShadowSettings.DistanceCoefficients[2];
    m_fShadowDistances[4] = farDist;

    // Practical split scheme (Zhang et al., parallel-split shadow maps): blend logarithmic splits, which give every
    // cascade the same texel density on screen, with uniform ones. Fixed fractions leave the last cascade covering most
    // of the shadow distance at a much coarser texel size than the one before it.
    const float lambda = gShadowSettings.GetFloat( "SplitLambda" );
    if ( lambda > 0.0f )
    {
        const int   count     = gShadowSettings.ShadowCascadeCount;
        const float splitNear = max( fNear, 0.1f );
        // ShadowDistance is meant from the ground: seen from the air (a plane hundreds of metres up) the ground starts
        // that far from the camera, and shadows measured from the camera would switch off as soon as you climb.
        const float heightAboveGround = max( TheCamera.GetPosition().z - TheCamera.CalculateGroundHeight( 0 ), 0.0f );
        const float splitFar  = max( min( gShadowSettings.GetFloat( "ShadowDistance" ) + heightAboveGround, farDist ), splitNear + 1.0f );
        for ( int i = 1; i <= count; i++ )
        {
            const float t = static_cast<float>( i ) / count;
            m_fShadowDistances[i] = lambda * splitNear * powf( splitFar / splitNear, t ) + ( 1.0f - lambda ) * ( splitNear + ( splitFar - splitNear ) * t );
        }
        for ( int i = count + 1; i < kMaxShadowCascades + 1; i++ )
            m_fShadowDistances[i] = splitFar;
    }
    for ( int i = 0; i < kMaxShadowCascades + 1; i++ )
        m_pLightCB->data.FadeDistances[i] = m_fShadowDistances[i];
}

void CShadowRenderer::QueueTextureReload()
{
    CRwD3D1XEngine* dxEngine = (CRwD3D1XEngine*)g_pRwCustomEngine;
    if ( m_bRequiresReloading )
    {
        m_pShadowCamera->zBuffer->width = gShadowSettings.Size * gShadowSettings.ShadowCascadeCount;
        m_pShadowCamera->zBuffer->height = gShadowSettings.Size;
        dxEngine->m_pRastersToReload.push_back( m_pShadowCamera->zBuffer );
    }
    if ( m_bRequiresReloading || dxEngine->m_bScreenSizeChanged )
        m_bForceFullUpdate = true;
}

void CShadowRenderer::DrawDebug()
{
    /* for (int i = 0; i < 4; i++)
     {
         for (size_t j = 0; j < 7; j++)
         {

         }

     }*/
}

tinyxml2::XMLElement * ShadowSettingsBlock::Save( tinyxml2::XMLDocument * doc )
{
    // Shadow settings node.
    auto shadowSettingsNode = SettingsBlock::Save( doc );

    shadowSettingsNode->SetAttribute( "Size", gShadowSettings.Size );
    shadowSettingsNode->SetAttribute( "ShadowCascadeCount", gShadowSettings.ShadowCascadeCount );

    for ( int i = 0; i < kMaxShadowCascades; i++ )
    {
        auto shadowCascadesSettingsNode = doc->NewElement( "Cascade" );
        shadowCascadesSettingsNode->SetAttribute( "ID", i + 1 );
        if ( i != 0 )
            shadowCascadesSettingsNode->SetAttribute( "StartDistanceMultiplier", gShadowSettings.DistanceCoefficients[i - 1] );
        shadowCascadesSettingsNode->SetAttribute( "BiasCoefficients", gShadowSettings.BiasCoefficients[i] );

        shadowSettingsNode->InsertEndChild( shadowCascadesSettingsNode );
    }

    return shadowSettingsNode;
}

void ShadowSettingsBlock::Load( const tinyxml2::XMLDocument & doc )
{
    SettingsBlock::Load( doc );
    auto shadowSettingsNode = doc.FirstChildElement( m_sName.c_str() );
    // A settings file without a Shadows element has to keep the defaults instead of
    // dereferencing a missing node. The defaults are re-established rather than left as they are,
    // so a reload with the element removed restores them instead of keeping whatever the earlier
    // load or the tweak bar left in Size, the coefficient tables and ShadowCascadeCount.
    if ( shadowSettingsNode == nullptr )
    {
        Reset();
    }
    else
    {
        // Size is a divisor for the cascade-count clamp and for the texel size, and it sizes the shadow
        // raster, so a settings file value below the tweak bar's minimum is rejected here as well.
        Size = (UINT)max( shadowSettingsNode->IntAttribute( "Size", 1024 ), 16 );
        ShadowCascadeCount = max( min( shadowSettingsNode->IntAttribute( "ShadowCascadeCount", 3 ), kMaxShadowCascades ), 1 );

        // Read up to 4 cascade parameter sets in document order. Each node is keyed by its ID
        // attribute, which has to be range checked before it indexes the coefficient tables.
        int parsedCascades = 0;
        for ( auto shadowCascadeNode = shadowSettingsNode->FirstChildElement( "Cascade" );
              shadowCascadeNode != nullptr && parsedCascades < kMaxShadowCascades;
              shadowCascadeNode = shadowCascadeNode->NextSiblingElement( "Cascade" ) )
        {
            const int id = shadowCascadeNode->IntAttribute( "ID" );
            // An out of range ID cannot index the tables, so the node is skipped without spending one of the
            // four cascade slots: charging it one would drop a valid cascade behind it.
            if ( id < 1 || id > kMaxShadowCascades )
                continue;
            if ( id != 1 )
                DistanceCoefficients[id - 2] = shadowCascadeNode->FloatAttribute( "StartDistanceMultiplier", DistanceCoefficients[id - 2] );
            BiasCoefficients[id - 1] = shadowCascadeNode->FloatAttribute( "BiasCoefficients", BiasCoefficients[id - 1] );
            parsedCascades++;
        }
    }

    // Both branches choose the two values that size the atlas, so the device limit and the rebuild request
    // belong to this shared tail rather than to the file branch: the restored defaults size the atlas exactly
    // as a file value does, and a reload that changed either value leaves the existing raster at its old extent
    // while every consumer reads the new ones. The atlas is Size * ShadowCascadeCount wide by Size tall, so the
    // limit the constructor and the tweak bar size callback enforce has to be re-applied here as well.
    int maxTexSize = 0;
    const bool deviceLimitKnown =
        g_pRwCustomEngine != nullptr && g_pRwCustomEngine->GetMaxTextureSize( maxTexSize ) && maxTexSize >= 16;
    if ( deviceLimitKnown )
        Size = (UINT)max( min( (int)Size, maxTexSize / ShadowCascadeCount ), 16 );
    // The rebuild multiplies the two values into the raster width, so it is only requested once that product is
    // device bounded. The query needs an engine and DllMain loads the file before one exists, so a missing engine
    // leaves the constructor to it. Only the renderer created after startup is reachable here, which is why the
    // whole chain is null checked.
    if ( deviceLimitKnown && g_pDeferredRenderer != nullptr &&
         g_pDeferredRenderer->m_pShadowRenderer != nullptr )
        g_pDeferredRenderer->m_pShadowRenderer->m_bRequiresReloading = true;
}

void ShadowSettingsBlock::Reset()
{
    SettingsBlock::Reset();
    // Shadows
    Size = 1024;
    BiasCoefficients[0] = 0.003f;
    BiasCoefficients[1] = 0.003f;
    BiasCoefficients[2] = 0.003f;
    BiasCoefficients[3] = 0.003f;
    DistanceCoefficients[0] = 0.01f;
    DistanceCoefficients[1] = 0.15f;
    DistanceCoefficients[2] = 0.45f;
    ShadowCascadeCount = 3;
}
void TW_CALL SetShadowSizeCallback( const void *value, void *clientData )
{
    // A failed device query leaves maxTexSize indeterminate, and dividing that would clamp the atlas size to
    // whatever the stack slot held, so the limit only applies when the query succeeded.
    int maxTexSize = 0;
    gShadowSettings.Size = (UINT)max( *(int*)value, 16 );
    if ( g_pRwCustomEngine != nullptr && g_pRwCustomEngine->GetMaxTextureSize( maxTexSize ) && maxTexSize >= 16 )
        gShadowSettings.Size = (UINT)max( min( (int)gShadowSettings.Size, maxTexSize / gShadowSettings.ShadowCascadeCount ), 16 );
    g_pDeferredRenderer->m_pShadowRenderer->m_bRequiresReloading = true;
}
void TW_CALL GetShadowSizeCallback( void *value, void *clientData )
{
    *(int*)value = gShadowSettings.Size;
}
void TW_CALL SetCascadeCountCallback( const void *value, void *clientData )
{
    // Same reason as SetShadowSizeCallback: an uninitialised maxTexSize would divide into the cascade count.
    int maxTexSize = 0;
    gShadowSettings.ShadowCascadeCount = *(int*)value;
    if ( g_pRwCustomEngine != nullptr && g_pRwCustomEngine->GetMaxTextureSize( maxTexSize ) && maxTexSize >= 16 )
        gShadowSettings.ShadowCascadeCount = min( gShadowSettings.ShadowCascadeCount, maxTexSize / (int)max( gShadowSettings.Size, 16u ) );
    gShadowSettings.ShadowCascadeCount = max( min( gShadowSettings.ShadowCascadeCount, kMaxShadowCascades ), 1 );
    g_pDeferredRenderer->m_pShadowRenderer->m_bRequiresReloading = true;
}
void TW_CALL GetCascadeCountCallback( void *value, void *clientData )
{
    *(int*)value = gShadowSettings.ShadowCascadeCount;
}
void ShadowSettingsBlock::InitGUI( TwBar * bar )
{
    SettingsBlock::InitGUI( bar );
    int maxTexSize = 0;
    // AntTweakBar keeps reading the definition text after this call returns and its implementation is not part
    // of this tree, so the buffer is held in static storage for the same reason the cascade range below is.
    static std::string shadowSettings;
    shadowSettings = "min=16 max=";
    if ( !g_pRwCustomEngine->GetMaxTextureSize( maxTexSize ) )
        shadowSettings += "1024";
    else
        shadowSettings += to_string( max( maxTexSize / 2, 16 ) );
    shadowSettings += " step=2 help='Shadow map size, higher - less pixelation/blurry shadows, lower - higher performance' group=Shadows";
    TwAddVarCB( bar, "Cascade Size", TwType::TW_TYPE_UINT32,
                SetShadowSizeCallback, GetShadowSizeCallback, nullptr, shadowSettings.c_str() );

    // AntTweakBar parses the definition text it is given here, but its implementation is not part of this tree, so the
    // buffer outlives the call the same way the literal it replaced did.
    static const std::string cascadeRange = "min=1 max=" + to_string( kMaxShadowCascades ) + " help='meh' group=Shadows";
    TwAddVarCB( bar, "Cascade count", TwType::TW_TYPE_INT32,
                SetCascadeCountCallback, GetCascadeCountCallback, nullptr, cascadeRange.c_str() );

    TwAddVarRW( bar, "Distance multipier 1", TwType::TW_TYPE_FLOAT,
                &DistanceCoefficients[0],
                " min=0.00001 max=1.0 step=0.00001 group=Cascade_1 " );

    TwAddVarRW( bar, "Distance multipier 2", TwType::TW_TYPE_FLOAT,
                &DistanceCoefficients[1],
                " min=0.00001 max=1.0 step=0.00001  group=Cascade_2 " );

    TwAddVarRW( bar, "Distance multipier 3", TwType::TW_TYPE_FLOAT,
                &DistanceCoefficients[2],
                " min=0.00001 max=1.0 step=0.00001  group=Cascade_3 " );

    TwAddVarRW( bar, "Bias 0", TwType::TW_TYPE_FLOAT,
                &BiasCoefficients[0],
                " min=0.000001 max=1.0 step=0.000001 group=Cascade_0 " );


    TwAddVarRW( bar, "Bias 1", TwType::TW_TYPE_FLOAT,
                &BiasCoefficients[1],
                " min=0.0001 max=1.0 step=0.00001 group=Cascade_1 " );


    TwAddVarRW( bar, "Bias 2", TwType::TW_TYPE_FLOAT,
                &BiasCoefficients[2],
                " min=0.0001 max=1.0 step=0.00001 group=Cascade_2 " );


    TwAddVarRW( bar, "Bias 3", TwType::TW_TYPE_FLOAT,
                &BiasCoefficients[3],
                " min=0.0001 max=1.0 step=0.00001 group=Cascade_3 " );

    TwDefine( " Settings/Cascade_0   group=Shadows label='1st Cascade'" );
    TwDefine( " Settings/Cascade_1   group=Shadows label='2nd Cascade'" );
    TwDefine( " Settings/Cascade_2   group=Shadows label='3rd Cascade'" );
    TwDefine( " Settings/Cascade_3   group=Shadows label='4th Cascade'" );
}
