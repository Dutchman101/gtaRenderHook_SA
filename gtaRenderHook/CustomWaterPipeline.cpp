// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "CustomWaterPipeline.h"
#include "D3DRenderer.h"
#include "CDebug.h"
#include "D3D1XShader.h"
#include "D3D1XStateManager.h"
#include "D3D1XRenderBuffersManager.h"
#include "RwD3D1XEngine.h"
WaterSettingsBlock gWaterSettings;
// The upload buffers are created once at a fixed size and mapped every frame, so these sizes are the one
// source for both the creation below and the fit check in RenderWater.
static const UINT kWaterVertexBufferBytes = 0x40000;
static const UINT kWaterIndexBufferBytes = 20000;

CCustomWaterPipeline::CCustomWaterPipeline() :
#ifndef DebuggingShaders
    CD3D1XPipeline( "SACustomWater" )
#else
    CD3D1XPipeline( L"SACustomWater" )
#endif // !DebuggingShaders
{
    gWaterSettings.m_aShaderPointers.push_back( m_pPS );
    ID3D11Device* pd3dDevice = GET_D3D_DEVICE;
    D3D11_INPUT_ELEMENT_DESC layout[] =
    {
        { "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT,		0, 0,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",		0, DXGI_FORMAT_R32G32B32_FLOAT,		0, 12,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",		0, DXGI_FORMAT_R8G8B8A8_UNORM,		0, 24,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",	0, DXGI_FORMAT_R32G32_FLOAT,		0, 28,	D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    UINT numElements = ARRAYSIZE( layout );
    ID3DBlob* vsBlob = m_pVS->getBlob();
    // Create the input layout
    if ( vsBlob == nullptr )
        g_pDebug->printError( "Water input layout skipped: the vertex shader did not compile." );
    else
    {
        const HRESULT layoutResult = pd3dDevice->CreateInputLayout( layout, numElements, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_pVertexLayout );
        if ( FAILED( layoutResult ) )
            g_pDebug->printError( "Water input layout: CreateInputLayout failed with " + std::to_string( layoutResult ) + "." );
    }


    D3D11_BUFFER_DESC bd;
    ZeroMemory( &bd, sizeof( bd ) );
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = kWaterVertexBufferBytes;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = 0;
    bd.StructureByteStride = 0;

    const HRESULT vertexBufferResult = pd3dDevice->CreateBuffer( &bd, nullptr, &m_pVertexBuffer );
    if ( FAILED( vertexBufferResult ) )
        g_pDebug->printError( "Water upload vertex buffer: CreateBuffer failed with " + std::to_string( vertexBufferResult ) + "." );

    ZeroMemory( &bd, sizeof( bd ) );
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = kWaterIndexBufferBytes;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = 0;
    bd.StructureByteStride = 0;

    const HRESULT indexBufferResult = pd3dDevice->CreateBuffer( &bd, nullptr, &m_pIndexBuffer );
    if ( FAILED( indexBufferResult ) )
        g_pDebug->printError( "Water upload index buffer: CreateBuffer failed with " + std::to_string( indexBufferResult ) + "." );
    m_pDS = new CD3D1XDomainShader( "shaders/SACustomWater.hlsl", "DS" );
    m_pHS = new CD3D1XHullShader( "shaders/SACustomWater.hlsl", "HS" );
}


CCustomWaterPipeline::~CCustomWaterPipeline()
{
    delete m_pDS;
    delete m_pHS;
    if ( m_pIndexBuffer ) m_pIndexBuffer->Release();
    if ( m_pVertexBuffer ) m_pVertexBuffer->Release();
    if ( m_pVertexLayout ) m_pVertexLayout->Release();
}

void CCustomWaterPipeline::RenderWater( RwIm3DVertex * verticles, UINT vertexCount, USHORT * indices, UINT indexCount )
{
    if ( verticles == nullptr || vertexCount == 0 || indices == nullptr || indexCount == 0 )
        return;
    if ( m_pVertexBuffer == nullptr || m_pIndexBuffer == nullptr )
    {
        g_pDebug->printError( "Water geometry dropped: the pipeline upload buffers were not created." );
        return;
    }
    // The buffers are mapped as one contiguous range of their creation size, so geometry that does not fit
    // would be copied past the end of the mapped region. The limit is compared as a count rather than as a
    // byte size so the comparison cannot wrap for any count the unsigned parameters can hold.
    if ( vertexCount > kWaterVertexBufferBytes / sizeof( RwIm3DVertex ) ||
         indexCount > kWaterIndexBufferBytes / sizeof( RwImVertexIndex ) )
    {
        g_pDebug->printError( "Water geometry dropped: it does not fit the pipeline fixed upload buffers." );
        return;
    }
    m_pWaterRaster = ( *(RwRaster**)0xC228A8 );
    m_pWaterWakeRaster = ( *(RwRaster**)0xC228B8 );
    g_pRenderBuffersMgr->UpdateWorldMatrix( nullptr );
    auto devContext = GET_D3D_CONTEXT;
    g_pRenderBuffersMgr->SetMatrixBuffer();
    {
        D3D11_MAPPED_SUBRESOURCE mappedResource;
        ZeroMemory( &mappedResource, sizeof( D3D11_MAPPED_SUBRESOURCE ) );
        //	Disable GPU access to the vertex buffer data.
        const HRESULT vertexMapResult = devContext->Map( m_pVertexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource );
        if ( FAILED( vertexMapResult ) || mappedResource.pData == nullptr )
        {
            // A map that succeeded but produced no pointer still holds the buffer, and the next frame
            // cannot map it again until it is released.
            if ( SUCCEEDED( vertexMapResult ) )
                devContext->Unmap( m_pVertexBuffer, 0 );
            g_pDebug->printError( "Water geometry dropped: the vertex buffer could not be mapped." );
            return;
        }
        //	Update the vertex buffer here.
        memcpy( mappedResource.pData, verticles, sizeof( RwIm3DVertex )*vertexCount );
        //	Reenable GPU access to the vertex buffer data.
        devContext->Unmap( m_pVertexBuffer, 0 );

        ZeroMemory( &mappedResource, sizeof( D3D11_MAPPED_SUBRESOURCE ) );
        //	Disable GPU access to the index buffer data.
        const HRESULT indexMapResult = devContext->Map( m_pIndexBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource );
        if ( FAILED( indexMapResult ) || mappedResource.pData == nullptr )
        {
            if ( SUCCEEDED( indexMapResult ) )
                devContext->Unmap( m_pIndexBuffer, 0 );
            g_pDebug->printError( "Water geometry dropped: the index buffer could not be mapped." );
            return;
        }
        //	Update the index buffer here.
        memcpy( mappedResource.pData, &indices[0], sizeof( RwImVertexIndex )*indexCount );
        //	Reenable GPU access to the index buffer data.
        devContext->Unmap( m_pIndexBuffer, 0 );
    }

    g_pStateMgr->SetInputLayout( m_pVertexLayout );

    UINT stride = sizeof( RwIm3DVertex );
    UINT offset = 0;
    g_pStateMgr->SetVertexBuffer( m_pVertexBuffer, stride, offset );
    g_pStateMgr->SetIndexBuffer( m_pIndexBuffer );
    g_pStateMgr->SetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST );
    g_pStateMgr->SetRaster( m_pWaterRaster );
    g_pStateMgr->SetTextureAdressUV(
        RwTextureAddressMode::rwTEXTUREADDRESSWRAP );
    g_pStateMgr->SetAlphaBlendEnable( true );
    g_pStateMgr->SetRaster( m_pWaterWakeRaster, 5 );
    //g_pStateMgr->SetFillMode(D3D11_FILL_WIREFRAME);
    m_pVS->Set();
    m_pPS->Set();
    m_pDS->Set();
    m_pHS->Set();
    g_pStateMgr->FlushStates();
    GET_D3D_RENDERER->DrawIndexed( indexCount, 0, 0 );
    m_pDS->ReSet();
    m_pHS->ReSet();
    //g_pStateMgr->SetFillMode(D3D11_FILL_SOLID);
}
void TW_CALL ReloadWaterShadersCallBack( void *value )
{
    gWaterSettings.m_bShaderReloadRequired = true;
}

void WaterSettingsBlock::InitGUI( TwBar * bar )
{
    SettingsBlock::InitGUI( bar );
    TwAddButton( bar, "Reload water shaders", ReloadWaterShadersCallBack, nullptr, "group=Water" );
}
