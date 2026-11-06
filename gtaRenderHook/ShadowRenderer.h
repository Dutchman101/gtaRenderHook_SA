#pragma once
#include "RwVectorMath.h"
#include "SettingsHolder.h"
#include "AntTweakBar.h"
#include "D3D1XConstantBuffer.h"

// One bound shared by the cascade arrays and by every writer of ShadowCascadeCount. The settings
// file, the tweak bar and Reset all assign that count, so a bound applied at only some of them
// lets the others index past the arrays below.
constexpr int kMaxShadowCascades = 4;

// Slack added past the light box's own depth when the shadow camera's far clip is set. The caster culling planes
// have to reach the same distance, so both the camera and the culling read this one value instead of repeating the
// number and drifting apart.
constexpr float kShadowFarClipMargin = 25.0f;

struct CBShadows
{
    RwMatrix ViewProj[kMaxShadowCascades];
    float	FadeDistances[5];
    int		ShadowSize;
    int		CascadeCount;
    float	pad;
    float	ShadowBias[4];
};

/*!
    \class CShadowRenderer
    \brief Cascaded shadow map renderer.

    This class responsible for rendering shadow maps.
*/
class CShadowRenderer
{
public:
    CShadowRenderer();
    ~CShadowRenderer();

    void DirectionalLightTransform( RwCamera* mainCam, const RW::V3d & lightDir, int shadowCascade );
    RW::V3d CalculateCameraPos( RwCamera* mainCam, const RW::V3d & lightDir, int shadowCascade );
    void CalculateFrustumPoints( RwReal fNear, RwReal fFar, RwCamera* camera, RW::V3d* points );
    void RenderShadowToBuffer( int cascade, void( *render )( int cascade ) );
    /*!
        Binds the shadow cbuffer, switches the sampler to border addressing and puts the cascade zBuffer on
        PS SRV slot 4. Every caller has to pair it with UnsetShadowBuffer(): the slot and the sampler mode
        are shared with the deferred G-buffer and post-processing passes, and a pass that inherits either
        one reads the shadow atlas or clamps its own UVs at the border instead of wrapping them.
    */
    void SetShadowBuffer() const;
    /*!
        Reverses SetShadowBuffer(): restores the sampler address modes it captured and releases PS SRV slot 4.
    */
    void UnsetShadowBuffer() const;
    void CalculateShadowDistances( RwReal fNear, RwReal fFar );
    void QueueTextureReload();
    void DrawDebug();
    /*!
        Chooses the cascades to re-render this frame (FarCascadeUpdateInterval). Every cascade is re-rendered when
        there were no shadows last frame (m_bShadowHistoryValid) or the light direction changed.
    */
    void SelectCascadesToUpdate( const RW::V3d &lightDir );
    bool IsCascadeUpdated( int cascade ) const { return m_bCascadeUpdated[cascade]; }
    bool AllCascadesUpdated() const;
    RwCamera* m_pShadowCamera = nullptr;
    float     m_fShadowDistances[kMaxShadowCascades + 1]{};
    bool	m_bShadowsRendered = false;
    // shadows were rendered last frame, so cascades kept from it are still valid
    bool	m_bShadowHistoryValid = false;
    bool	m_bRequiresReloading = false;
    /*!
        Light aligned bounding box for each shadow cascade
    */
    static RW::BBox m_LightBBox[kMaxShadowCascades];
    static RwV3d m_LightPos[kMaxShadowCascades];
    static RW::Matrix m_LightSpaceMatrix[kMaxShadowCascades];
    static RW::Matrix m_InvLightSpaceMatrix[kMaxShadowCascades];
    static RW::V3d m_vFrustumCorners[kMaxShadowCascades][8];
private:
    void ClearCascade( int cascade );
    bool    m_bCascadeUpdated[kMaxShadowCascades]{ true, true, true, true };
    bool    m_bForceFullUpdate = true;
    UINT    m_uiFrame = 0;
    RW::V3d m_vLastLightDir{ 0, 0, 0 };
    RW::V3d m_vLastCameraPos{ 0, 0, 0 };
    RW::V3d m_vLastCameraAt{ 0, 0, 0 };
    class CD3D1XShader* m_pDepthClearPS = nullptr;
    RwMatrix* m_pLightViewProj;
    CD3D1XConstantBuffer<CBShadows>* m_pLightCB;
    // Sampler address modes in force before SetShadowBuffer overrode them, kept so UnsetShadowBuffer can put
    // back exactly what the surrounding pass expects rather than a guess. Mutable because both the bind and
    // the unbind are logically const: they change render state, not the shadow renderer. Seeded with the mode
    // the state manager actually starts on, so an unbind that ran before any bind would still restore it.
    mutable RwTextureAddressMode m_eSavedAddressU = rwTEXTUREADDRESSWRAP;
    mutable RwTextureAddressMode m_eSavedAddressV = rwTEXTUREADDRESSWRAP;
    mutable RwTextureAddressMode m_eSavedAddressW = rwTEXTUREADDRESSWRAP;
};

class ShadowSettingsBlock : public SettingsBlock
{
public:
    ShadowSettingsBlock()
    {
        m_sName = "Shadows";
        m_aFields["Enable"] = new ToggleSField( "Enable", false, false, false, m_sName, true );
        m_aFields["CullPerCascade"] = new ToggleSField( "CullPerCascade", false, false, false, m_sName );
        m_aFields["ScanShadowsBehindPlayer"] = new ToggleSField( "ScanShadowsBehindPlayer", false, false, false, m_sName, true );
        m_aFields["MaxDrawDistance"] = new FloatSField( "MaxDrawDistance", false, false, false, m_sName, 500.0f, 5.0f, 30000.0f, 5.0f );
        m_aFields["LodShadowsMinDistance"] = new FloatSField( "LodShadowsMinDistance", false, false, false, m_sName, 150.0f, 2.0f, 3000.0f, 1.0f );
        m_aFields["MaxSectorsAroundPlayer"] = new UIntSField( "MaxSectorsAroundPlayer", false, false, false, m_sName, 3, 1, 10, 1 );
        // Cascade splits: 0 = the cascades' StartDistanceMultiplier values; above 0, the practical split scheme, which
        // blends logarithmic (1) and uniform (0) splits up to ShadowDistance
        m_aFields["SplitLambda"] = new FloatSField( "SplitLambda", false, false, false, m_sName, 0.0f, 0.0f, 1.0f, 0.01f );
        // where shadows end with SplitLambda, in metres, plus the camera's height above the ground (limited by MaxDrawDistance)
        m_aFields["ShadowDistance"] = new FloatSField( "ShadowDistance", false, false, false, m_sName, 450.0f, 10.0f, 30000.0f, 5.0f );
        // cascades from the third on are re-rendered every this many frames, staggered (1 = every frame)
        m_aFields["FarCascadeUpdateInterval"] = new UIntSField( "FarCascadeUpdateInterval", false, false, false, m_sName, 1, 1, 8, 1 );
        Reset();
    }
    tinyxml2::XMLElement* Save( tinyxml2::XMLDocument* doc );
    void Load( const tinyxml2::XMLDocument &doc );
    void Reset();
    void InitGUI( TwBar* );
public:
    UINT Size;
    float DistanceCoefficients[3];
    float BiasCoefficients[4];
    int ShadowCascadeCount;
};
extern ShadowSettingsBlock gShadowSettings;
