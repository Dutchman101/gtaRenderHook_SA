#include "stdafx.h"
#include "TemporalAA.h"
#include "RwRenderEngine.h"
#include "RwD3D1XEngine.h"
#include "D3DRenderer.h"
#include "D3D1XTexture.h"
#include "D3D1XBaseTexture.h"
#include "FullscreenQuad.h"
#include "D3D1XStateManager.h"
#include "D3D1XRenderBuffersManager.h"
#include <game_sa\CScene.h>
#include <game_sa\CCamera.h>
#include "RenderHookAPI.h"
#include "CDebug.h"
CD3D1XShader *CTemporalAA::m_pTemporalAA = nullptr;
RwRaster *CTemporalAA::m_pAccumRasters[2] = { nullptr, nullptr };
RwV3d CTemporalAA::m_vPrevCamPos = { 0,0,0 };
RwGraphicsMatrix CTemporalAA::m_mPrevView = {};
int CTemporalAA::m_nCurrentAccRaster = 0;
int CTemporalAA::m_nCurrentJitterSample = 0;
CD3D1XConstantBuffer<CBTemporalAA>* CTemporalAA::m_pTAABuffer = nullptr;
TAASettingsBlock gTAASettingsBlock;
bool CTemporalAA::m_bFirstTimeRender = true;
float CTemporalAA::m_fJitterX = 0.0f;
float CTemporalAA::m_fJitterY = 0.0f;
float CTemporalAA::m_fPrevJitterX = 0.0f;
float CTemporalAA::m_fPrevJitterY = 0.0f;
RwGraphicsMatrix CTemporalAA::m_mView = {};
RwGraphicsMatrix CTemporalAA::m_mProjection = {};
RwRaster *CTemporalAA::m_pObjectMotionRaster = nullptr;
static RHTemporalAAHandler g_externalTAA = nullptr;
// main camera view-projection without jitter, last frame (object motion vectors)
static RwGraphicsMatrix g_prevViewProj{};
static bool g_hasPrevViewProj = false;
// an external TAA handled the last frame (shaders cut transparency out instead of dithering it then)
static bool g_externalTAAHandled = false;

bool CTemporalAA::ExternalTAAActive() { return g_externalTAAHandled && gTAASettingsBlock.GetToggleField( "EnableTAA" ); }
bool CTemporalAA::HasExternalHandler() { return g_externalTAA != nullptr; }

void __cdecl RHSetTemporalAAHandler( RHTemporalAAHandler handler )
{
    g_externalTAA = handler;
}

static ID3D11ShaderResourceView *RasterSRV( RwRaster *raster )
{
    auto d3d1xRaster = raster ? GetD3D1XRaster( raster ) : nullptr;
    auto viewable    = d3d1xRaster ? dynamic_cast<ID3D1XShaderViewable *>( d3d1xRaster->resourse ) : nullptr;
    return viewable ? viewable->GetShaderResourceView() : nullptr;
}

// AddRef'd texture behind a raster, or nullptr
static ID3D11Texture2D *RasterTexture2D( RwRaster *raster )
{
    ID3D11ShaderResourceView *srv = RasterSRV( raster );
    if ( !srv )
        return nullptr;
    ID3D11Resource * resource = nullptr;
    ID3D11Texture2D *texture  = nullptr;
    srv->GetResource( &resource );
    if ( resource )
    {
        resource->QueryInterface( __uuidof( ID3D11Texture2D ), reinterpret_cast<void **>( &texture ) );
        resource->Release();
    }
    return texture;
}

// Gives the frame to a registered external TAA (see RenderHookAPI.h). true = it wrote the output.
static bool RunExternalTAA( RwRaster *input, RwRaster *output, RwRaster *gBuff1, float jitterX, float jitterY,
                            const RwGraphicsMatrix &view, const RwGraphicsMatrix &projection )
{
    if ( !g_externalTAA )
        return false;
    RHTemporalAAFrame frame{};
    frame.size     = sizeof( frame );
    frame.context  = GET_D3D_CONTEXT;
    frame.input    = RasterTexture2D( input );
    frame.output   = RasterTexture2D( output );
    frame.gbuffer1 = RasterSRV( gBuff1 );
    frame.objectMotion = RasterSRV( CTemporalAA::m_pObjectMotionRaster );
    if ( frame.gbuffer1 )
        frame.gbuffer1->AddRef();
    if ( frame.objectMotion )
        frame.objectMotion->AddRef();
    frame.jitterX  = jitterX;
    frame.jitterY  = jitterY;
    memcpy( frame.view, &view, sizeof( frame.view ) );
    memcpy( frame.projection, &projection, sizeof( frame.projection ) );
    bool handled   = frame.input && frame.output && frame.gbuffer1 && g_externalTAA( &frame );
    if ( frame.input )
        frame.input->Release();
    if ( frame.output )
        frame.output->Release();
    if ( frame.gbuffer1 )
        frame.gbuffer1->Release();
    if ( frame.objectMotion )
        frame.objectMotion->Release();
    if ( handled )
    {
        CD3D1XShader::InvalidateStateCache();
        g_pStateMgr->InvalidateStateCache();
    }
    return handled;
}
void CTemporalAA::Init()
{
    // A second start-up would otherwise overwrite the handles a previous one owns and leak them, so it starts from empty.
    Shutdown();
    CreateAccumRasters();
    // Fresh accumulation rasters hold no history, so the next Render has to substitute the current input for it.
    m_bFirstTimeRender = true;
    // The stored view-projection still describes the frame from before the restart, so without this the next
    // JitterProjMatrix reports the camera's whole jump as object motion instead of seeding it from this frame.
    g_hasPrevViewProj = false;
    // Cleared as a whole first, because the diagonal writes below touch only those four entries and a start-up after
    // a rendered frame would otherwise hand Render the previous camera's inverse view with only its diagonal replaced.
    // All three are set before the two constructions below, because the second of them is what makes CanAccumulate
    // report that the pass can run, and Render reads this history from that point on.
    m_mPrevView = {};
    m_mPrevView.m[0].x = 1.0f;
    m_mPrevView.m[1].y = 1.0f;
    m_mPrevView.m[2].z = 1.0f;
    m_mPrevView.m[3].w = 1.0f;
    m_pTemporalAA = new CD3D1XPixelShader( "shaders/TemporalAA.hlsl", "TAA_PS" );
    m_pTAABuffer = new CD3D1XConstantBuffer<CBTemporalAA>();
    m_pTAABuffer->SetDebugName( "TemporalAACB" );
}

void CTemporalAA::Shutdown()
{
    // Released and cleared together, so a repeated shutdown does not release them twice and a reload after this does
    // not read a released raster.
    for ( auto &raster : m_pAccumRasters )
    {
        if ( raster != nullptr )
        {
            RwRasterDestroy( raster );
            raster = nullptr;
        }
    }
    delete m_pTemporalAA;
    m_pTemporalAA = nullptr;
    delete m_pTAABuffer;
    m_pTAABuffer = nullptr;
}

// Fills only the rasters that are missing, so a start-up that could not create them and a reload retry cannot disagree
// about the size, the format or the diagnostic, and neither overwrites a handle that is already live.
void CTemporalAA::CreateAccumRasters()
{
    for ( auto &raster : m_pAccumRasters )
    {
        if ( raster == nullptr )
            raster = RwRasterCreate( RsGlobal.maximumWidth, RsGlobal.maximumHeight, 32, rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT1555 );
    }
    if ( m_pAccumRasters[0] == nullptr || m_pAccumRasters[1] == nullptr )
        g_pDebug->printError( "Temporal AA accumulation rasters are incomplete: the built-in TAA pass is disabled." );
}

// Render and JitterProjMatrix both read this, so the two cannot disagree about whether a frame can be accumulated.
bool CTemporalAA::CanAccumulate()
{
    return m_pTemporalAA != nullptr && m_pTAABuffer != nullptr &&
           m_pAccumRasters[0] != nullptr && m_pAccumRasters[1] != nullptr;
}

void CTemporalAA::JitterProjMatrix()
{
    // No display mode exists before the engine establishes one, and the reciprocal of a zero size is infinite, which
    // reached the projection as a non-finite offset. Zero leaves that frame unjittered and touches nothing else.
    float rcx = RsGlobal.maximumWidth >= 2 ? 1.0f / RsGlobal.maximumWidth : 0.0f;
    float rcy = RsGlobal.maximumHeight >= 2 ? 1.0f / RsGlobal.maximumHeight : 0.0f;
    float Scale = gTAASettingsBlock.GetFloat( "SubPixelScale" );
    float x8aa_X[8] = {
        -7.0f / 8.0f,
        -5.0f / 8.0f,
        -1.0f / 8.0f,
        3.0f / 8.0f,
        5.0f / 8.0f,
        7.0f / 8.0f,
        1.0f / 8.0f,
        -3.0f / 8.0f };
    float x8aa_Y[8] = {
        1.0f / 8.0f,
        -5.0f / 8.0f,
        -3.0f / 8.0f,
        -7.0f / 8.0f,
        -1.0f / 8.0f,
        7.0f / 8.0f,
        3.0f / 8.0f,
        5.0f / 8.0f };
    RwGraphicsMatrix *projTransformRef = (RwGraphicsMatrix*)&RwD3D9D3D9ProjTransform;

    // An external handler accumulates on its own and needs the jitter (see RenderHookAPI.h), so the accumulation
    // handles only gate this when nothing else will accumulate the frame.
    const bool taaEnabled = gTAASettingsBlock.GetToggleField( "EnableTAA" ) &&
                            ( HasExternalHandler() || CanAccumulate() );
    const UINT subPixelCount = min( max( gTAASettingsBlock.GetUInt( "SubPixelCount" ), 1u ), 8u );
    m_mView       = *(RwGraphicsMatrix *)&RwD3D9D3D9ViewTransform;
    m_mProjection = *projTransformRef;
    m_fPrevJitterX = m_fJitterX;
    m_fPrevJitterY = m_fJitterY;
    if ( taaEnabled )
    {
        // advance first: "x = ( x++ ) % n" assigned the old value back, so the sample never changed (no jitter at all)
        m_nCurrentJitterSample = ( m_nCurrentJitterSample + 1 ) % subPixelCount;
        float SampleX = ( x8aa_X[m_nCurrentJitterSample] ) * rcx;
        float SampleY = ( x8aa_Y[m_nCurrentJitterSample] ) * rcy;
        m_fJitterX = SampleX * Scale;
        m_fJitterY = SampleY * Scale;
        projTransformRef->m[2].x += m_fJitterX;
        projTransformRef->m[2].y += m_fJitterY;
    }
    else
    {
        g_externalTAAHandled = false;
        m_bFirstTimeRender   = true;
        m_fJitterX           = 0.0f;
        m_fJitterY           = 0.0f;
    }

    g_pRenderBuffersMgr->UpdateViewProjMatricles( RwD3D9D3D9ViewTransform, *(RwMatrix*)projTransformRef );

    // object motion vectors: this camera's view-projection without the jitter, against last frame's
    RwGraphicsMatrix viewProj;
    g_pRenderBuffersMgr->Multipy4x4Matrices( &viewProj, &m_mView, &m_mProjection );
    if ( !g_hasPrevViewProj )
        g_prevViewProj = viewProj;
    g_pRenderBuffersMgr->SetMotionCamera( *(RwMatrix *)&g_prevViewProj, m_fJitterX, m_fJitterY, ExternalTAAActive() );
    g_prevViewProj    = viewProj;
    g_hasPrevViewProj = true;
}

void CTemporalAA::SaveViewMatrix()
{
    // Public, so the camera test belongs here as well as at the call sites, and the stored matrix keeps its previous
    // value when there is no camera to take one from.
    if ( Scene.m_pRwCamera == nullptr )
        return;
    RwMatrix* camMatrix = RwFrameGetLTM( static_cast<RwFrame*>( Scene.m_pRwCamera->object.object.parent ) );
    // rw used inverse of camera frame matrix with negative column-vector
    RwMatrixInvert( (RwMatrix*)&m_mPrevView, camMatrix );
    m_mPrevView.m[0].x = -m_mPrevView.m[0].x;
    m_mPrevView.m[1].x = -m_mPrevView.m[1].x;
    m_mPrevView.m[2].x = -m_mPrevView.m[2].x;
    m_mPrevView.m[3].x = -m_mPrevView.m[3].x;
    m_mPrevView.m[0].w = 0.0f;
    m_mPrevView.m[1].w = 0.0f;
    m_mPrevView.m[2].w = 0.0f;
    m_mPrevView.m[3].w = 1.0f;
}

void CTemporalAA::Render( RwRaster * input, RwRaster* output, RwRaster* gBuff1, RwRaster* gBuff2 )
{
    // The view matrix below and SaveViewMatrix on two of the three exits both go through the game camera, which does
    // not exist until the scene has one. The copy keeps the output defined, because the caller swaps its targets
    // whether or not this pass wrote anything.
    if ( Scene.m_pRwCamera == nullptr )
    {
        CFullscreenQuad::Copy( input, nullptr, output );
        return;
    }
    RwV3d* curCamPos = RwMatrixGetPos( &Scene.m_pRwCamera->viewMatrix );
    g_externalTAAHandled = RunExternalTAA( input, output, gBuff1, m_fJitterX, m_fJitterY, m_mView, m_mProjection );
    if ( g_externalTAAHandled )
    {
        // our history is stale now; start it fresh if the external TAA hands the frames back
        m_bFirstTimeRender = true;
        m_vPrevCamPos      = *curCamPos;
        SaveViewMatrix();
        return;
    }
    // A start-up that did not finish leaves no shader, no constant buffer, or no accumulation raster, and all of them
    // are used below. Passing the frame through keeps the output defined instead of leaving it blank.
    if ( !CanAccumulate() )
    {
        CFullscreenQuad::Copy( input, nullptr, output );
        return;
    }
    m_pTAABuffer->data.PrevView = m_mPrevView;
    m_pTAABuffer->data.MovementVec = { m_vPrevCamPos.x - curCamPos->x, m_vPrevCamPos.y - curCamPos->y, m_vPrevCamPos.z - curCamPos->z };
    const int frameCount = max( gTAASettingsBlock.GetInt( "FrameCount" ), 1 );
    m_pTAABuffer->data.BlendFactor = ( frameCount - 1.0f ) / ( 1.0f * frameCount );
    m_pTAABuffer->data.MBMaxSpeed = gTAASettingsBlock.GetFloat( "MBMaxSpeed" );
    m_pTAABuffer->data.MBMinPixelDistance = gTAASettingsBlock.GetFloat( "MBMinPixelDistance" );
    m_pTAABuffer->data.MBEdgeScale = gTAASettingsBlock.GetFloat( "MBEdgeScale" );
    m_pTAABuffer->data.MBCenterScale = gTAASettingsBlock.GetFloat( "MBCenterScale" );
    m_pTAABuffer->Update();
    // No depth buffer is bound: the full-screen quad runs with depth testing off, so passing the scene
    // z-buffer here only added a redundant depth-stencil bind to every frame.
    g_pRwCustomEngine->SetRenderTargets( &m_pAccumRasters[m_nCurrentAccRaster], nullptr, 1 );
    g_pStateMgr->FlushRenderTargets();
    g_pStateMgr->SetConstantBufferPS( m_pTAABuffer, 9 );
    g_pStateMgr->SetRaster( input );
    g_pStateMgr->SetRaster( m_bFirstTimeRender ? input : m_pAccumRasters[1 - m_nCurrentAccRaster], 1 );
    g_pStateMgr->SetRaster( gBuff1, 2 );
    g_pStateMgr->SetRaster( gBuff2, 3 );
    g_pStateMgr->SetRaster( m_pObjectMotionRaster, 4 );
    m_pTemporalAA->Set();
    CFullscreenQuad::Draw();
    g_pStateMgr->SetRaster( nullptr );
    g_pStateMgr->SetRaster( nullptr, 1 );
    g_pStateMgr->SetRaster( nullptr, 2 );
    g_pStateMgr->SetRaster( nullptr, 3 );
    g_pStateMgr->SetRaster( nullptr, 4 );
    CFullscreenQuad::Copy( m_pAccumRasters[m_nCurrentAccRaster], nullptr, output );


    m_nCurrentAccRaster = 1 - m_nCurrentAccRaster;
    m_bFirstTimeRender = false;
    m_vPrevCamPos = *curCamPos;
    SaveViewMatrix();
}

void CTemporalAA::QueueTextureReload()
{
    // static_cast cannot yield null for a non-null pointer, so the test below covers only the engine being absent, which
    // happens before start-up and after unload. The queueing that runs before this one in the same frame converts the
    // same pointer without a test, so this hardens its own unit rather than being a guard that fires on its own.
    CRwD3D1XEngine *dxEngine = static_cast<CRwD3D1XEngine *>(g_pRwCustomEngine);
    if ( dxEngine == nullptr || !dxEngine->m_bScreenSizeChanged )
        return;
    // A start-up that ran before a display mode existed could not create the rasters and reported that the pass is
    // disabled, and the resize below only queues rasters that exist. A mode change is the first point at which a valid
    // size is known to exist, so retrying here is what keeps such a start-up from disabling the pass for the session.
    if ( m_pAccumRasters[0] == nullptr || m_pAccumRasters[1] == nullptr )
        CreateAccumRasters();
    for ( auto &raster : m_pAccumRasters )
    {
        // A start-up that never created a raster leaves nothing to resize, and the resize below would write through
        // the null pointer.
        if ( raster == nullptr )
            continue;
        raster->width  = RsGlobal.maximumWidth;
        raster->height = RsGlobal.maximumHeight;
        dxEngine->m_pRastersToReload.push_back( raster );
    }
    // The engine reallocates both accum rasters later in this frame, which drops whatever history they held,
    // so the next Render would blend against an undefined previous frame. Marking it as the first frame makes
    // Render substitute the current input for that history.
    m_bFirstTimeRender = true;
}

void TAASettingsBlock::Load( const tinyxml2::XMLDocument & doc )
{
    SettingsBlock::Load( doc );
}

void TAASettingsBlock::InitGUI( TwBar * bar )
{
    SettingsBlock::InitGUI( bar );
}
