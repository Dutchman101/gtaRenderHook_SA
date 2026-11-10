#pragma once
#include "RwVectorMath.h"
struct CBReflections
{
    RwMatrix View[6];
};
class CCubemapReflectionRenderer
{
  public:
    CCubemapReflectionRenderer( int size );
    ~CCubemapReflectionRenderer();
    void RenderToCubemap( void ( *renderCB )() );
    void RenderOneFace( void ( *renderCB )(), int id, float angleA,
                        RwV3d axisA, float angleB, RwV3d axisB, RwV3d camPos /*,const RW::V3d&At, const RW::V3d&Up, const RW::V3d&Right, RW::V3d&Pos*/ );
    void SetCubemap();

  public:
    // The destructor reads both through this handle and runs even when the constructor stopped part way through, so
    // neither may start out indeterminate.
    RwCamera *m_pReflCamera{};
    RwFrame * m_pReflCameraFrame{};
    int       m_nCubemapSize;

  private:
    // Faces in the environment cube: the view array, both loops over it and the per-face bound all read this.
    const static int cubemapFaceCount = 6;
    ID3D11Buffer *   m_pReflCB{};
    // Set only once every view the pass needs exists, so the pass can tell a usable cubemap from one whose
    // creation stopped part way through.
    bool             m_bResourcesReady{};
    ID3D11Texture2D *g_pEnvMap{}; // Environment map
    ID3D11RenderTargetView
        *g_pEnvMapRTV{}; // Render target view for the alpha map
    ID3D11RenderTargetView
        *g_apEnvMapOneRTV[cubemapFaceCount]{}; // one render target view per
                                               // face of the env map
    ID3D11ShaderResourceView
        *g_pEnvMapSRV{}; // Shader resource view for the cubic env map
};
