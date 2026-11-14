#include "stdafx.h"
#include <algorithm>
#include <cstdio>
#include "CloudRendering.h"
#include "D3D1X3DTexture.h"
#include "D3D1XShader.h"
#include "D3D1XShaderDefines.h"
#include "FastNoiseSIMD.h"
#include "D3D1XStateManager.h"
#include "D3D1XTextureMemoryManager.h"
#include "CDebug.h"
#include <game_sa\CWeather.h>
#include "FullscreenQuad.h"
#include "RwRenderEngine.h"
#include <game_sa\CScene.h>
#include <game_sa\CTimeCycle.h>
#include <game_sa\CTimer.h>
#include "D3DRenderer.h"
#include "LightManager.h"

namespace
{
// The engine keeps these terms in 0..1 and the shaders multiply them by fixed coefficents, so a value
// outside that range arrives as a density nothing downstream bounds: the atmosphere density alone is 3.0 plus
// sandstorm times 7. One rule for all of them keeps them from drifting apart.
constexpr float kMinUnitRange = 0.0f;
constexpr float kMaxUnitRange = 1.0f;

// A term outside the range the shaders were written for, including one that is not a number.
template <typename T> bool IsOutOfUnitRange( T value )
{
    const auto asFloat = static_cast<float>( value );
    return !( asFloat >= kMinUnitRange && asFloat <= kMaxUnitRange );
}

template <typename T> float ClampUnit( T value )
{
    const auto asFloat = static_cast<float>( value );
    // A value that is not a number fails both comparisons and std::clamp would pass it straight through,
    // so it is mapped to the low end here rather than reaching the buffer as something that is not a number.
    if ( !( asFloat >= kMinUnitRange ) )
        return kMinUnitRange;
    return std::clamp( asFloat, kMinUnitRange, kMaxUnitRange );
}

// One flag per term, because each term leaving the range is its own broken state and a flag over the whole
// group would let whichever term went out first hide the rest.
bool g_bReportedWeatherFoggyness = false;
bool g_bReportedWeatherRain      = false;
bool g_bReportedWeatherWetRoads  = false;
bool g_bReportedWeatherSandstorm = false;
bool g_bReportedCloudCoverage    = false;

// Bounds one term into 0..1 and reports the first time the value it was handed was outside that range, naming
// both the value received and the value served. It returns what the caller must store, so the buffer always
// holds the value the report named. The flag is tested before the message is built, so a caller that runs
// every frame allocates nothing in the steady state, and it is set before the report so a report that fails
// cannot repeat every frame.
template <typename T>
float ClampUnitOnce( bool &alreadyReported, const char *block, const char *term, T value )
{
    const float bounded = ClampUnit( value );
    if ( IsOutOfUnitRange( value ) && !alreadyReported )
    {
        alreadyReported = true;
        char message[128] = {};
        std::snprintf( message, sizeof( message ), "%s: %s=%.9g is outside 0..1, served %.9g.", block, term,
                       static_cast<double>( value ), static_cast<double>( bounded ) );
        g_pDebug->printError( message );
    }
    return bounded;
}
} // namespace

CD3D1XShader *CCloudRendering::m_pRenderCloudsPS         = nullptr;
CD3D1XShader *CCloudRendering::m_pVolumetricCombinePS          = nullptr;
RwRaster *    CCloudRendering::m_pCloudsRaster         = nullptr;
RwRaster *    CCloudRendering::m_pNoise3DRaster     = nullptr;
CD3D1XConstantBuffer<CBCloudRendering> *CCloudRendering::m_pCloudRenderingCB =
    nullptr;
CD3D1XConstantBuffer<CBWeatherState> *CCloudRendering::m_pWeatherCB = nullptr;
bool                            CCloudRendering::m_bRequiresReloading = false;
CloudRenderingSettingsBlock gCloudRenderingSettings;
void CCloudRendering::Init()
{
    m_pRenderCloudsPS = new CD3D1XPixelShader(
        "shaders/CloudRendering.hlsl", "RenderCloudsPS",
        &gCloudRenderingSettings.m_ShaderDefineList );
    m_pVolumetricCombinePS = new CD3D1XPixelShader(
        "shaders/CloudRendering.hlsl", "CloudsCombinePS",
        &gCloudRenderingSettings.m_ShaderDefineList );
    float renderingScale =
        gCloudRenderingSettings.GetFloat( "CloudsRenderingScale" );
    m_pCloudsRaster =
        RwRasterCreate( ( RwInt32 )( RsGlobal.maximumWidth * renderingScale ),
                        ( RwInt32 )( RsGlobal.maximumHeight * renderingScale ),
                        32, rwRASTERTYPECAMERATEXTURE | rwRASTERFORMAT1555 );
    // Volume cloud generation
    GenerateCloudsTexture();

    m_pCloudRenderingCB = new CD3D1XConstantBuffer<CBCloudRendering>();
    m_pWeatherCB        = new CD3D1XConstantBuffer<CBWeatherState>();
    gCloudRenderingSettings.m_aShaderPointers.push_back( m_pRenderCloudsPS );
    gCloudRenderingSettings.m_aShaderPointers.push_back(
        m_pVolumetricCombinePS );
    gDebugSettings.DebugRenderTargetList.push_back( m_pCloudsRaster );
}

void CCloudRendering::Shutdown()
{
    delete m_pCloudRenderingCB;
    delete m_pWeatherCB;
    delete m_pRenderCloudsPS;
    delete m_pVolumetricCombinePS;
    if ( m_pCloudsRaster )
        RwRasterDestroy( m_pCloudsRaster );
    if ( m_pNoise3DRaster )
        RwRasterDestroy( m_pNoise3DRaster );

    // The handles outlive this call, so they are cleared here rather than left set to memory that was just released. A
    // second shutdown then releases nothing, and the reload path sees the same absent handles it sees before setup.
    m_pCloudRenderingCB = nullptr;
    m_pWeatherCB        = nullptr;
    m_pRenderCloudsPS   = nullptr;
    m_pVolumetricCombinePS = nullptr;
    m_pCloudsRaster     = nullptr;
    m_pNoise3DRaster    = nullptr;
}

void CCloudRendering::UpdateWeatherState()
{
    // The buffer is created in Init and cleared in Shutdown, and this runs from the render path, so the only
    // way to arrive without one is a frame drawn inside that window. Leaving the last bound state alone there
    // costs a frame of stale weather instead of a fault.
    if ( m_pWeatherCB == nullptr )
        return;

    m_pWeatherCB->data.Foggyness =
        ClampUnitOnce( g_bReportedWeatherFoggyness, "Weather block", "Foggyness", CWeather::Foggyness );
    m_pWeatherCB->data.Rain =
        ClampUnitOnce( g_bReportedWeatherRain, "Weather block", "Rain", CWeather::Rain );
    m_pWeatherCB->data.WetRoads =
        ClampUnitOnce( g_bReportedWeatherWetRoads, "Weather block", "WetRoads", CWeather::WetRoads );
    m_pWeatherCB->data.Sandstorm =
        ClampUnitOnce( g_bReportedWeatherSandstorm, "Weather block", "Sandstorm", CWeather::Sandstorm );
    m_pWeatherCB->Update();
    // Bound once per frame from the start of the deferred output rather than from the cloud pass, so that a
    // flavour shader set reading these terms in any later pass sees the current frame's values. This tree
    // declares the block and reads none of it, so here the binding changes nothing. b11 is the register
    // Globals.hlsl gives cbuffer WeatherState, and no other buffer uses it.
    g_pStateMgr->SetConstantBufferPS( m_pWeatherCB, 11 );
}

void CCloudRendering::UpdateCloudState()
{
    // Written once per frame from the top of the deferred output rather than from the cloud pass, because the
    // cloud pass sits behind a toggle and a flavour shader set can read members of this block from a pass that
    // runs while the toggle is off. This tree reads the block only from the cloud pass, so the write is inert
    // here. b12 is the register Globals.hlsl gives cbuffer Clouds, and the cloud pass also binds the same block
    // to b10 for sets that declare that slot.
    if ( m_pCloudRenderingCB == nullptr )
        return;

    // The cloud shader picks its noise exponent with lerp(45, 0.5, CloudCoverage), so this term has to stay
    // inside the 0..1 that remap was written for.
    m_pCloudRenderingCB->data.CloudCoverage =
        ClampUnitOnce( g_bReportedCloudCoverage, "Cloud block", "CloudCoverage", CWeather::CloudCoverage );
    m_pCloudRenderingCB->data.CloudsColor[0] =
        CTimeCycle::m_CurrentColours.m_nFluffyCloudsBottomRed / 255.0f;
    m_pCloudRenderingCB->data.CloudsColor[1] =
        CTimeCycle::m_CurrentColours.m_nFluffyCloudsBottomGreen / 255.0f;
    m_pCloudRenderingCB->data.CloudsColor[2] =
        CTimeCycle::m_CurrentColours.m_nFluffyCloudsBottomBlue / 255.0f;
    m_pCloudRenderingCB->data.CloudsColor[3] =
        CTimeCycle::m_CurrentColours.m_fCloudAlpha/255.0f;
    m_pCloudRenderingCB->data.CloudStartHeight = 
        gCloudRenderingSettings.GetFloat( "CloudStartHeight" );
    m_pCloudRenderingCB->data.CloudEndHeight =
        gCloudRenderingSettings.GetFloat( "CloudEndHeight" );
    m_pCloudRenderingCB->data.CloudSpeed =
        gCloudRenderingSettings.GetFloat( "CloudSpeed" );
    // The two layer heights are configuration, not engine state: this buffer carries them verbatim. A tree that
    // divides by their separation and a tree that reads them as an animation-phase offset both get the configured
    // value, and neither is rewritten here.
    
    m_pCloudRenderingCB->data.WindDirX = CWeather::WindDir.x;
    m_pCloudRenderingCB->data.WindDirY = CWeather::WindDir.y;
    m_pCloudRenderingCB->data.WindDirZ = CWeather::WindDir.z;
    constexpr auto time_interval       = 1.0f / ( 0.0005f ) * 3000.0f;
    if ( m_pCloudRenderingCB->data.Time < time_interval )
        m_pCloudRenderingCB->data.Time += CTimer::ms_fTimeStep;
    else
        m_pCloudRenderingCB->data.Time = m_pCloudRenderingCB->data.Time -
                                         time_interval + CTimer::ms_fTimeStep;
    
    m_pCloudRenderingCB->Update();

    g_pStateMgr->SetConstantBufferPS( m_pCloudRenderingCB, 12 );
}

bool CCloudRendering::RenderVolumetricEffects( RwRaster *normalsDepth,
                                               RwRaster *from, RwRaster *to )
{
    // The caller blits this pass's target unconditionally, so it adopts that target only when this returns
    // true. Reporting the refusal leaves the caller on the pass that wrote its output this frame. Both rasters
    // and both shaders are created in Init, and a creation that fails leaves its handle null for the rest of
    // the session, so every handle the draws below dereference is checked here and a success verdict means
    // this pass wrote its target. A shader that failed to compile keeps its object but binds no stage, and a
    // draw with no pixel shader writes nothing, so the compile result is checked for the same reason. The
    // cloud constant buffer is left out of this gate because UpdateCloudState owns it. The bind below guards it
    // on its own, and Init creates it alongside the rasters this gate already checks, so it is present whenever
    // the gate passes.
    if ( m_pCloudsRaster == nullptr || m_pNoise3DRaster == nullptr ||
         m_pRenderCloudsPS == nullptr || m_pVolumetricCombinePS == nullptr ||
         !m_pRenderCloudsPS->IsValid() || !m_pVolumetricCombinePS->IsValid() )
    {
        // Each reason leads somewhere different, so each one names itself instead of sharing one message that
        // would read as covering the rest. printError keeps one line per distinct message per run, so a
        // resource that was never created does not repeat every frame.
        if ( m_pCloudsRaster == nullptr )
            g_pDebug->printError( "Cloud pass skipped: the cloud raster was not created." );
        else if ( m_pNoise3DRaster == nullptr )
            g_pDebug->printError( "Cloud pass skipped: the noise texture was not created." );
        else if ( m_pRenderCloudsPS == nullptr || m_pVolumetricCombinePS == nullptr )
            g_pDebug->printError( "Cloud pass skipped: a cloud shader object is absent." );
        else
            g_pDebug->printError( "Cloud pass skipped: a cloud shader did not compile." );
        return false;
    }
    // A shader set that declares cbuffer Clouds at b10 reads b10 during this pass. Binding both slots here is
    // what keeps the register change additive: a set that reads b12 reads b12, a set that reads b10 reads b10,
    // and neither reads the light array, which the light pass binds to b10 earlier in the frame. It is one
    // buffer uploaded once, so offering it on the second bank and putting that bank back afterwards costs two
    // slot writes and no bandwidth.
    if ( m_pCloudRenderingCB != nullptr )
        g_pStateMgr->SetConstantBufferPS( m_pCloudRenderingCB, 10 );
    // First render all volumetric lighting(sun light, volume streetlights,
    // clouds etc.)
    g_pRwCustomEngine->SetRenderTargets( &m_pCloudsRaster, nullptr, 1 );
    g_pStateMgr->FlushRenderTargets();
    g_pStateMgr->SetRaster( normalsDepth, 1 );
    g_pStateMgr->SetTextureAdressUV(
        RwTextureAddressMode::rwTEXTUREADDRESSWRAP );
    g_pStateMgr->SetRaster( m_pNoise3DRaster, 3 );

    m_pRenderCloudsPS->Set();
    CFullscreenQuad::Draw();
    // Than combine them with
    g_pRwCustomEngine->SetRenderTargets( &to, nullptr, 1 );
    g_pStateMgr->FlushRenderTargets();
    g_pStateMgr->SetRaster( from );
    g_pStateMgr->SetRaster( m_pCloudsRaster, 2 );
    m_pVolumetricCombinePS->Set();

    CFullscreenQuad::Draw();

    g_pStateMgr->SetRaster( nullptr );
    g_pStateMgr->SetRaster( nullptr, 1 );
    g_pStateMgr->SetRaster( nullptr, 2 );
    g_pStateMgr->SetRaster( nullptr, 3 );

    // b10 goes back to the light array it held before this pass, so a shader set that reads that bank anywhere
    // else in the frame sees what it saw before the cloud block was offered there. The light buffer is created
    // in CLightManager::Init and cleared in Shutdown, so an absent one simply leaves the bank as it is.
    auto light_buffer = CLightManager::GetBuffer();
    if ( light_buffer != nullptr )
        g_pStateMgr->SetConstantBufferPS( light_buffer, 10 );

    return true;
}

void CCloudRendering::QueueTextureReload() {
    CRwD3D1XEngine *dxEngine = (CRwD3D1XEngine *)g_pRwCustomEngine;

    if ( m_bRequiresReloading || dxEngine->m_bScreenSizeChanged )
    {
        float renderingScale =
            gCloudRenderingSettings.GetFloat( "CloudsRenderingScale" );
        // A raster that was never created has no size to change, and the screen size change that reaches here is the same
        // one that can make creation fail. The reload pass runs on its own and clears nothing this call owns, so skipping
        // the queue here leaves it with no entry to process for this effect and nothing left holding a stale size.
        if ( m_pCloudsRaster == nullptr )
            return;

        m_pCloudsRaster->width =
            (int)( RsGlobal.maximumWidth * renderingScale );
        m_pCloudsRaster->height =
            (int)( RsGlobal.maximumHeight * renderingScale );
        dxEngine->m_pRastersToReload.push_back( m_pCloudsRaster );
    }
}

void CCloudRendering::GenerateCloudsTexture() {
    m_pNoise3DRaster = RwRasterCreate(
        128, 128, 128, ( rwRASTERTYPECAMERATEXTURE + 1 ) | rwRASTERFORMAT565 );
    // The lookup below is an offset added to the raster, so a raster that was not created is not a null to compare
    // against afterwards, and the texture would be written into the space just past the null address.
    if ( m_pNoise3DRaster == nullptr )
        return;
    auto raster_impl = GetD3D1XRaster( m_pNoise3DRaster );

    std::vector<RwV4d> pixel_array;
    pixel_array.resize( 128 * 128 * 128 );
    FastNoiseSIMD *noise_gen = FastNoiseSIMD::NewFastNoiseSIMD();
    float *noiseSet = noise_gen->GetCellularSet( 0, 0, 0, 128, 128, 128, 2.0F );
    float *noiseSet2 =
        noise_gen->GetCellularSet( 0, 0, 0, 128, 128, 128, 4.0F );
    float *noiseSet3 =
        noise_gen->GetCellularSet( 0, 0, 0, 128, 128, 128, 8.0F );
    float *perlinSet =
        noise_gen->GetPerlinFractalSet( 0, 0, 0, 128, 128, 128, 8.0F );
    size_t idx = 0;
    for ( size_t x = 0; x < 128; x++ )
    {
        for ( size_t y = 0; y < 128; y++ )
        {
            size_t y_slice_offset = 128 * 128 * x;
            for ( size_t z = 0; z < 128; z++ )
            {
                size_t z_slice_offset                            = 128 * y;
                pixel_array[y_slice_offset + z_slice_offset + z] = {
                    ( 1.0f - noiseSet[idx] ), ( 1.0f - noiseSet2[idx] ),
                    ( 1.0f - noiseSet3[idx] ), perlinSet[idx++]};
            }
        }
    }
    FastNoiseSIMD::FreeNoiseSet( noiseSet );
    FastNoiseSIMD::FreeNoiseSet( noiseSet2 );
    FastNoiseSIMD::FreeNoiseSet( noiseSet3 );
    FastNoiseSIMD::FreeNoiseSet( perlinSet );

    D3D11_SUBRESOURCE_DATA texture_data{};
    texture_data.pSysMem          = pixel_array.data();
    texture_data.SysMemPitch      = 128 * sizeof( RwV4d );
    texture_data.SysMemSlicePitch = 128 * 128 * sizeof( RwV4d );
    raster_impl->resourse =
        new CD3D1X3DTexture( m_pNoise3DRaster, &texture_data );

    CD3D1XTextureMemoryManager::AddNew( raster_impl->resourse );
}

void CloudRenderingSettingsBlock::Load( const tinyxml2::XMLDocument &doc ) 
{
    SettingsBlock::Load( doc );
    m_ShaderDefineList.AddDefine(
        "CLOUD_RM_STEPS", std::to_string( gCloudRenderingSettings.GetUInt(
                              "CloudsRaymarchingSteps" ) ) );
    m_ShaderDefineList.AddDefine(
        "CLOUD_TO_SUN_RM_STEPS", std::to_string( gCloudRenderingSettings.GetUInt(
                              "SunRaymarchingSteps" ) ) );
    
}

void TW_CALL ReloadCloudShadersCallBack( void *value )
{
    gCloudRenderingSettings.m_bShaderReloadRequired = true;
    gCloudRenderingSettings.m_ShaderDefineList.Reset();
    gCloudRenderingSettings.m_ShaderDefineList.AddDefine(
        "CLOUD_RM_STEPS",
        std::to_string( gCloudRenderingSettings.GetUInt(
                              "CloudsRaymarchingSteps" ) ) );
    gCloudRenderingSettings.m_ShaderDefineList.AddDefine(
        "CLOUD_TO_SUN_RM_STEPS",
        std::to_string(
            gCloudRenderingSettings.GetUInt( "SunRaymarchingSteps" ) ) );
}
void TW_CALL ReloadCloudTexturesCallBack( void *value )
{
    CCloudRendering::m_bRequiresReloading = true;
}

void CloudRenderingSettingsBlock::InitGUI( TwBar *mainBar )
{
    SettingsBlock::InitGUI( mainBar );
    TwAddButton( mainBar, "Reload cloud shaders", ReloadCloudShadersCallBack,
                 nullptr,
                 "group=CloudRendering" );
    TwAddButton( mainBar, "Reload cloud textures", ReloadCloudTexturesCallBack,
                 nullptr,
                 "group=CloudRendering" );
}
