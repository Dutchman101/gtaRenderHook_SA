// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "CustomSeabedPipeline.h"
#include "D3DRenderer.h"
#include "CDebug.h"
#include "D3D1XShader.h"
#include "D3D1XStateManager.h"
#include "DeferredRenderer.h"
#include "D3D1XRenderBuffersManager.h"
#include "RwD3D1XEngine.h"

// The upload buffers are created once at a fixed size and mapped every frame, so these sizes are the one
// source for both the creation below and the fit check in RenderSeabed.
static const UINT kSeabedVertexBufferBytes = 0x40000;
static const UINT kSeabedIndexBufferBytes = 20000;

CCustomSeabedPipeline::CCustomSeabedPipeline() :
#ifndef DebuggingShaders
    CDeferredPipeline( "SACustomSeabed", GET_D3D_FEATURE_LVL >= D3D_FEATURE_LEVEL_11_0 )
#else
    CDeferredPipeline( L"SACustomSeabed", GET_D3D_FEATURE_LVL >= D3D_FEATURE_LEVEL_11_0 )
#endif // !DebuggingShaders
{
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
        g_pDebug->printError( "Seabed input layout skipped: the vertex shader did not compile." );
    else
    {
        const HRESULT layoutResult = pd3dDevice->CreateInputLayout( layout, numElements, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_pVertexLayout );
        if ( FAILED( layoutResult ) )
            g_pDebug->printError( "Seabed input layout: CreateInputLayout failed with " + std::to_string( layoutResult ) + "." );
    }


    D3D11_BUFFER_DESC bd;
    ZeroMemory( &bd, sizeof( bd ) );
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = kSeabedVertexBufferBytes;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = 0;
    bd.StructureByteStride = 0;

    const HRESULT vertexBufferResult = pd3dDevice->CreateBuffer( &bd, nullptr, &m_pVertexBuffer );
    if ( FAILED( vertexBufferResult ) )
        g_pDebug->printError( "Seabed upload vertex buffer: CreateBuffer failed with " + std::to_string( vertexBufferResult ) + "." );

    ZeroMemory( &bd, sizeof( bd ) );
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = kSeabedIndexBufferBytes;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = 0;
    bd.StructureByteStride = 0;

    const HRESULT indexBufferResult = pd3dDevice->CreateBuffer( &bd, nullptr, &m_pIndexBuffer );
    if ( FAILED( indexBufferResult ) )
        g_pDebug->printError( "Seabed upload index buffer: CreateBuffer failed with " + std::to_string( indexBufferResult ) + "." );
    m_pSeaBedTexRaster = ( *(RwRaster**)0xC228B0 );
}

CCustomSeabedPipeline::~CCustomSeabedPipeline()
{
    if ( m_pIndexBuffer ) m_pIndexBuffer->Release();
    if ( m_pVertexBuffer ) m_pVertexBuffer->Release();
    if ( m_pVertexLayout ) m_pVertexLayout->Release();
}

void CCustomSeabedPipeline::RenderSeabed( RwIm3DVertex * verticles, UINT vertexCount, USHORT * indices, UINT indexCount )
{
    if ( verticles == nullptr || vertexCount == 0 || indices == nullptr || indexCount == 0 )
        return;
    if ( m_pVertexBuffer == nullptr || m_pIndexBuffer == nullptr )
    {
        g_pDebug->printError( "Seabed geometry dropped: the pipeline upload buffers were not created." );
        return;
    }
    // The buffers are mapped as one contiguous range of their creation size, so geometry that does not fit
    // would be copied past the end of the mapped region. The limit is compared as a count rather than as a
    // byte size so the comparison cannot wrap for any count the unsigned parameters can hold.
    if ( vertexCount > kSeabedVertexBufferBytes / sizeof( RwIm3DVertex ) ||
         indexCount > kSeabedIndexBufferBytes / sizeof( RwImVertexIndex ) )
    {
        g_pDebug->printError( "Seabed geometry dropped: it does not fit the pipeline fixed upload buffers." );
        return;
    }
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
            g_pDebug->printError( "Seabed geometry dropped: the vertex buffer could not be mapped." );
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
            g_pDebug->printError( "Seabed geometry dropped: the index buffer could not be mapped." );
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
    g_pStateMgr->SetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    g_pStateMgr->SetRaster( m_pSeaBedTexRaster );
    if ( m_uiDeferredStage == 3 || m_uiDeferredStage == 4 )
    {
        m_pVoxelVS->Set();
        m_pVoxelGS->Set();
    }
    else
        m_pVS->Set();
    if ( m_uiDeferredStage == 1 )
        m_pDeferredPS->Set();
    else if ( m_uiDeferredStage == 2 )
        m_pShadowPS->Set();
    else if ( m_uiDeferredStage == 3 )
        m_pVoxelPS->Set();
    else if ( m_uiDeferredStage == 4 )
        m_pVoxelEmmissivePS->Set();
    else
        m_pPS->Set();

    g_pStateMgr->FlushStates();
    GET_D3D_RENDERER->DrawIndexed( indexCount, 0, 0 );
}
