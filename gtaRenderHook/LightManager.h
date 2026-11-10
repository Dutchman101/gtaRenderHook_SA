#pragma once
#include "D3D1XStructuredBuffer.h"
#include "D3D1XConstantBuffer.h"
struct CLight
{
    RwV3d m_vPos;
    int   m_nLightType;
    RwV3d m_vColor;
    float m_fRange;
    RwV3d m_vDir;
    float m_fPadding;
};
struct LightsCB
{
    CLight m_aLights[16];
};
class CLightManager
{
public:
    static CLight m_aLights[1024];
private:
    // Counted only while m_pLightBuffer exists, so a count above zero always names lights the point light pass can
    // read. Private because that is what the point light gate's safety rests on, and the count is the only member
    // a writer outside this file could use to break it.
    static int m_nLightCount;
    static CD3D1XConstantBuffer<LightsCB>* m_pLightBuffer;
public:
    static void Init();
    static void Shutdown();
    static void Reset();
    static bool AddLight( const CLight &light );
    static void SortByDistance( const RwV3d& from );
    static void Update();
    static int GetLightCount();
    static CD3D1XConstantBuffer<LightsCB>* GetBuffer();
};

