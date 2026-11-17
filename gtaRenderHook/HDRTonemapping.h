#pragma once
#include "PostProcessEffect.h"
#include "SettingsHolder.h"
#include "D3D1XConstantBuffer.h"
#pragma warning( push, 0 )
#include <AntTweakBar.h>
#pragma warning( pop )
class CD3D1XShader;
constexpr auto NUM_TONEMAP_TEXTURES =
    3; // Number of stages in the 3x3 down-scaling for post-processing in PS
static const int ToneMappingTexSize = (int)pow( 3.0f, NUM_TONEMAP_TEXTURES - 1 );
// Address of the game's day/night blend weight, 0 for full day and 1 for full night. The game writes it
// from its custom building renderer update, and a frame loop that replaces that update has to publish it
// as well: every reader would otherwise keep the value it was initialised to, which is full night.
constexpr auto GAME_DAY_NIGHT_BALANCE_ADDR = 0x8D12C0;
struct CBPostProcess
{
    float	LumWhite;
    float	MiddleGray;
    float	pad[2];
};
class CHDRTonemapping :
    public CPostProcessEffect
{
public:
    CHDRTonemapping();
    ~CHDRTonemapping();
    virtual void Render( RwRaster* input, RwRaster* output, RwRaster* zbuffer );
private:
    CD3D1XShader* m_pTonemap;
    CD3D1XShader* m_pDownScale2x2_Lum;
    CD3D1XShader* m_pDownScale3x3;
    CD3D1XShader* m_pAdaptationPass;
    RwRaster*	m_pAdaptationRaster[2];
    RwRaster *    m_pToneMapRaster[NUM_TONEMAP_TEXTURES]{};
    UINT m_nCurrentAdaptationRaster = 0;
    CD3D1XConstantBuffer<CBPostProcess>* m_pPostFXBuffer;
};

// Tonemap settings block class
class TonemapSettingsBlock : public SettingsBlock
{
public:
    TonemapSettingsBlock()
    {
        m_sName = "Tonemap";
        m_aFields["EnableTonemap"] = new ToggleSField( "EnableTonemap", false, false, false, m_sName, true );
        m_aFields["UseGTAColorCorrection"] = new ToggleSField( "UseGTAColorCorrection", false, false, true, m_sName, true );
        m_aFields["LumWhiteDay"] = new FloatSField( "LumWhiteDay", false, false, false, m_sName, 1.25f, 0.0f, 10.0f, 0.005f );
        m_aFields["LumWhiteNight"] = new FloatSField( "LumWhiteNight", false, false, false, m_sName, 1.0f, 0.0f, 10.0f, 0.005f );
        m_aFields["MiddleGrayDay"] = new FloatSField( "MiddleGrayDay", false, false, false, m_sName, 0.55f, 0.0f, 10.0f, 0.005f );
        m_aFields["MiddleGrayNight"] = new FloatSField( "MiddleGrayNight", false, false, false, m_sName, 0.25f, 0.0f, 10.0f, 0.005f );
        Reset();
    }
    tinyxml2::XMLElement* Save( tinyxml2::XMLDocument* doc );
    void Load( const tinyxml2::XMLDocument &doc );
    void InitGUI( TwBar* );
public:
    float GetCurrentLumWhite();
    float GetCurrentMiddleGray();
};
extern TonemapSettingsBlock gTonemapSettings;
