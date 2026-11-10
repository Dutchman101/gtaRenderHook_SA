// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "LightManager.h"
#include "RwD3D1XEngine.h"
#include "D3DRenderer.h"
#include "D3D1XStateManager.h"
#include "RwMethods.h"
#include <game_sa\CCamera.h>

CLight CLightManager::m_aLights[1024];
CD3D1XConstantBuffer<LightsCB>* CLightManager::m_pLightBuffer = nullptr;
int CLightManager::m_nLightCount = 0;

void CLightManager::Init()
{
    // Started from empty so a second start-up does not overwrite the buffer a previous one owns and leak it.
    Shutdown();
    m_pLightBuffer = new CD3D1XConstantBuffer<LightsCB>();
}

void CLightManager::Shutdown()
{
    // Cleared as it is released: a consumer that reads the buffer afterwards gets a null rather than a handle to
    // released memory, and the count is cleared so the point light pass cannot work through the empty slot.
    delete m_pLightBuffer;
    m_pLightBuffer = nullptr;
    m_nLightCount = 0;
}

void CLightManager::Reset()
{
    m_nLightCount = 0;
}

bool CLightManager::AddLight( const CLight& light )
{
    // Lights are only counted while there is a buffer to hand them to, so a count above zero always means the point
    // light pass has something to read rather than an empty slot.
    if ( m_pLightBuffer == nullptr )
        return false;
    if ( m_nLightCount < 1024 )
    {
        m_aLights[m_nLightCount] = light;
        m_nLightCount++;
        return true;
    }
    else
        return false;
}

void CLightManager::SortByDistance( const RwV3d& from )
{
    // only this frame's lights: Reset() just zeroes the count, so the rest of the array holds lights from earlier frames
    sort( &m_aLights[0], &m_aLights[m_nLightCount], [&] ( const CLight &a, const CLight &b )
    {
        return ( rwV3D_Dist( a.m_vPos, from ) < rwV3D_Dist( b.m_vPos, from ) );
    } );
}

void CLightManager::Update()
{
    const int activeLightCount = min( m_nLightCount, 16 );
    for ( auto i = 0; i < 16; i++ )
    {
        // Only the first activeLightCount entries hold this frame's lights; the rest must be
        // cleared rather than carrying stale lights from earlier frames into the GPU buffer.
        m_pLightBuffer->data.m_aLights[i] = i < activeLightCount ? m_aLights[i] : CLight();
    }

    m_pLightBuffer->Update();
    g_pStateMgr->SetLightCount( activeLightCount );
}

CD3D1XConstantBuffer<LightsCB> * CLightManager::GetBuffer()
{
    return m_pLightBuffer;
}

int CLightManager::GetLightCount()
{
    return m_nLightCount;
}
