#pragma once
#include "D3D1XConstantBuffer.h"
#include "SettingsHolder.h"

class CD3D1XShader;

struct CBCloudRendering
{
    float WindDirX;
    float WindDirY;
    float WindDirZ;
    float Time;
    float CloudsColor[4];

    float CloudStartHeight;
    float CloudEndHeight;
    float CloudCoverage;
    float CloudSpeed;
};

// Globals.hlsl declares these same members in cbuffer Clouds. The size is checked here so a member added on one
// side without the other fails the build instead of quietly shifting every later field. Size is all it can
// check: a reorder of same-sized members keeps 48 and compiles, so the two member lists have to be kept in the
// same order by hand.
static_assert( sizeof( CBCloudRendering ) == 48,
               "CBCloudRendering must match the Clouds cbuffer in Globals.hlsl" );

// Engine weather in its own buffer, so a shader set can read it without also reading the cloud block. It is
// declared at b11, and nothing else in the shader tree uses that bank. cbuffer Clouds sits at b12 for the
// reason this block did not take b10: b10 carries the dynamic light array that Deferred.hlsl declares and the
// light pass binds there mid frame, so a member read from b10 after that point reads light data instead.
struct CBWeatherState
{
    float Foggyness;
    float Rain;
    float WetRoads;
    float Sandstorm;
};

// Four floats keep the buffer a whole number of float4 registers, so CD3D1XConstantBuffer::Update stays on its
// direct upload path. The size is checked here for the same reason as the cloud buffer above, and the same
// limit applies: the four members have to stay in the hlsl order by hand, because a reorder keeps 16.
static_assert( sizeof( CBWeatherState ) == 16,
               "CBWeatherState must match the WeatherState cbuffer in Globals.hlsl" );

class CCloudRendering
{
  public:
    static void Init();
    static void Shutdown();

    static bool RenderVolumetricEffects( RwRaster *normalsDepth, RwRaster *from, RwRaster *to );
    static void UpdateWeatherState();
    static void UpdateCloudState();
    static void QueueTextureReload();
    static void GenerateCloudsTexture();

    static bool m_bRequiresReloading;

  private:
    static CD3D1XShader *m_pRenderCloudsPS;
    static CD3D1XShader *m_pVolumetricCombinePS;
    static RwRaster *    m_pCloudsRaster;
    static RwRaster *    m_pNoise3DRaster;
    static CD3D1XConstantBuffer<CBCloudRendering> *m_pCloudRenderingCB;
    static CD3D1XConstantBuffer<CBWeatherState> *  m_pWeatherCB;
};

/*!
    Cloud rendering settings.
*/
class CloudRenderingSettingsBlock : public SettingsBlock
{
  public:
    CloudRenderingSettingsBlock()
    {
        m_sName = "CloudRendering";
        m_aFields["EnableCloudRendering"] =
            new ToggleSField( "EnableCloudRendering", false, false, false, m_sName, false );
        m_aFields["CloudStartHeight"] =
            new FloatSField( "CloudStartHeight", false, false, false, m_sName,
                             200.0f, 0.0f, 10000.0f, 1.f );
        m_aFields["CloudEndHeight"] =
            new FloatSField( "CloudEndHeight", false, false, false, m_sName,
                             400.0f, 0.0f, 10000.0f, 1.f );
        m_aFields["CloudSpeed"] =
            new FloatSField( "CloudSpeed", false, false, false, m_sName,
                             1.0f, 0.0f, 100.0f, 1.f );
        m_aFields["CloudsRenderingScale"] =
            new FloatSField( "CloudsRenderingScale", false, false, false,
                             m_sName, 0.95f, 0.25f, 4.0f, 0.01f );
        m_aFields["CloudsRaymarchingSteps"] =
            new UIntSField( "CloudsRaymarchingSteps", false, false, false,
                            m_sName, 16, 2, 256 );
        m_aFields["SunRaymarchingSteps"] =
            new UIntSField( "SunRaymarchingSteps", false, false, false,
                            m_sName, 2, 1, 32 );
        Reset();
    }
    void Load( const tinyxml2::XMLDocument &doc );
    void InitGUI( TwBar * );
};
extern CloudRenderingSettingsBlock gCloudRenderingSettings;