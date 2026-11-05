// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "D3D1XIm2DPipeline.h"
#include "CDebug.h"
#include "D3DRenderer.h"
#include "D3D1XShader.h"
#include "D3D1XStateManager.h"
#include "RwD3D1XEngine.h"
#include "D3D1XVertexDeclarationManager.h"

// Update takes the upload size as an int, so a batch whose byte count does not fit that parameter
// cannot be uploaded at all. Dividing the limit by the element size yields the largest count that
// fits, and the division itself cannot wrap.
static const size_t kIm2DMaxUploadVertices = static_cast<size_t>( (std::numeric_limits<int>::max )() ) / sizeof( RwIm2DVertex );
static const size_t kIm2DMaxUploadIndices = static_cast<size_t>( (std::numeric_limits<int>::max )() ) / sizeof( RwImVertexIndex );
// A triangle fan one vertex shorter than its vertex count expands to three vertices per triangle.
// The fan length is bounded by the upload limit divided by that factor, so the bound itself cannot
// wrap and the product formed afterwards stays inside the limit. size_t is 32 bits on this target,
// so comparing the product against the limit would let a large fan wrap to a small count.
static const size_t kIm2DMaxFanVertices = kIm2DMaxUploadVertices / 3;
static const size_t kIm2DMaxFanIndices = kIm2DMaxUploadIndices / 3;

CD3D1XIm2DPipeline::CD3D1XIm2DPipeline() :
#ifndef DebuggingShaders
    CD3D1XPipeline( "RwIm2D" )
#else
    CD3D1XPipeline( L"RwIm2D" )
#endif // !DebuggingShaders
{
    std::vector<D3D11_INPUT_ELEMENT_DESC> layout =
    {
        { "POSITION",	0, DXGI_FORMAT_R32G32B32A32_FLOAT,	0, 0,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",		0, DXGI_FORMAT_R8G8B8A8_UNORM,		0, 16,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",	0, DXGI_FORMAT_R32G32_FLOAT,		0, 20,	D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    m_pVertexDecl = new CD3D1XVertexDeclaration( layout, sizeof( RwIm2DVertex ), m_pVS );
    m_pVertexBuffer = new CD3D1XDynamicVertexBuffer( sizeof( RwIm2DVertex ), m_uiVertexCapacity );
    m_pIndexBuffer = new CD3D1XDynamicIndexBuffer( m_uiIndexCapacity );
    m_pVertexBuffer->SetDebugName( "Im2D_DynamicVB" );
    m_pIndexBuffer->SetDebugName( "Im2D_DynamicIB" );
}


CD3D1XIm2DPipeline::~CD3D1XIm2DPipeline()
{
    if ( m_pVertexBuffer )
    {
        delete m_pVertexBuffer;
        m_pVertexBuffer = nullptr;
    }
    if ( m_pIndexBuffer )
    {
        delete m_pIndexBuffer;
        m_pIndexBuffer = nullptr;
    }
    if ( m_pVertexDecl )
    {
        delete m_pVertexDecl;
        m_pVertexDecl = nullptr;
    }
}

// Sprite batches (e.g. coronas of plugins) can pass the initial 10000 vertices, so the buffers grow to
// fit the call instead of the upload being written past their end.
// Returns whether the buffers now cover the request. A growth that fails leaves the old buffer in place,
// and the upload clamps itself to it while the draw call keeps using the requested count, so the caller has
// to drop the batch when this reports failure.
bool CD3D1XIm2DPipeline::EnsureCapacity( unsigned int vertexCount, unsigned int indexCount )
{
    if ( vertexCount > m_uiVertexCapacity )
    {
        const unsigned int grownCapacity = max( vertexCount, m_uiVertexCapacity * 2 );
        // The replacement allocates, and that allocation is what reports a failed creation, so it is
        // taken before anything is released: a throw here must leave the member naming the buffer it
        // still owns rather than one that has already been freed.
        CD3D1XDynamicVertexBuffer* grownBuffer = nullptr;
        // The upload bound admits a count this 32 bit address space cannot serve, so the request is containable
        // but not satisfiable. Catching it keeps the failure inside the module: the member still names the buffer
        // it owns, the capacity is unchanged, and the false return is what makes the caller drop the batch.
        try
        {
            grownBuffer = new CD3D1XDynamicVertexBuffer( sizeof( RwIm2DVertex ), grownCapacity );
        }
        catch ( ... )
        {
            g_pDebug->printError( "Im2D vertex batch dropped: the vertex buffer could not grow to fit it." );
            return false;
        }
        // The state manager caches the bound buffer by pointer. Releasing the old buffer without
        // clearing that slot lets a recycled address compare equal and skip the rebind, leaving a
        // released ID3D11Buffer bound for the next draw.
        g_pStateMgr->SetVertexBuffer( nullptr, 0, 0 );
        delete m_pVertexBuffer;
        m_pVertexBuffer = grownBuffer;
        m_uiVertexCapacity = grownCapacity;
        m_pVertexBuffer->SetDebugName( "Im2D_DynamicVB" );
    }
    if ( indexCount > m_uiIndexCapacity )
    {
        const unsigned int grownCapacity = max( indexCount, m_uiIndexCapacity * 2 );
        CD3D1XDynamicIndexBuffer* grownBuffer = nullptr;
        // Same containment as the vertex buffer above, reported separately so the failing buffer is named.
        try
        {
            grownBuffer = new CD3D1XDynamicIndexBuffer( grownCapacity );
        }
        catch ( ... )
        {
            g_pDebug->printError( "Im2D index batch dropped: the index buffer could not grow to fit it." );
            return false;
        }
        // SetIndexBuffer compares the same cached pointer, so the slot is cleared for the same reason.
        g_pStateMgr->SetIndexBuffer( nullptr );
        delete m_pIndexBuffer;
        m_pIndexBuffer = grownBuffer;
        m_uiIndexCapacity = grownCapacity;
        m_pIndexBuffer->SetDebugName( "Im2D_DynamicIB" );
    }
    return true;
}

void CD3D1XIm2DPipeline::Draw( RwPrimitiveType prim, RwIm2DVertex* verticles, RwUInt32 vertexCount )
{
    if ( verticles == nullptr || vertexCount == 0 || ( prim == rwPRIMTYPETRIFAN && vertexCount < 3 ) )
        return;
    ID3D11DeviceContext* pImmediateContext = GET_D3D_CONTEXT;
    auto vertCount = vertexCount;
    // Fan input is expanded into an explicit triangle list below, so the topology has to follow
    // the expansion rather than the caller's original primitive type.
    RwPrimitiveType topologyPrim = prim;
    // dynamic vertex buffer update
    if ( prim != rwPRIMTYPETRIFAN )
    {
        if ( vertCount > kIm2DMaxUploadVertices )
        {
            g_pDebug->printError( "Im2D sprite batch dropped: its upload size does not fit the upload size parameter." );
            return;
        }
        if ( !EnsureCapacity( vertCount, 0 ) )
            return;
        m_pVertexBuffer->Update( &verticles[0], sizeof( RwIm2DVertex ) * vertCount );
    }
    else
    {
        // The fan length is checked before the expansion is multiplied out. Comparing the product
        // instead would let three per triangle wrap a 32 bit count, which would size the vector for
        // a short expansion while the loop below ran the full length and wrote past its end.
        if ( static_cast<size_t>( vertexCount - 2 ) > kIm2DMaxFanVertices )
        {
            g_pDebug->printError( "Im2D triangle fan batch dropped: its expanded upload size does not fit the upload size parameter." );
            return;
        }
        const size_t expandedCount = static_cast<size_t>( vertexCount - 2 ) * 3;
        std::vector<RwIm2DVertex> vertexArr;
        // The expansion is a transient host allocation, not a device resource, so it is taken before
        // any draw state changes. A batch that fits the bound above but cannot be expanded is dropped
        // here with the bound buffers and the cached state untouched, instead of ending the process.
        try
        {
            vertexArr.resize( expandedCount );
        }
        catch ( ... )
        {
            g_pDebug->printError( "Im2D triangle fan batch dropped: its vertex expansion could not be allocated." );
            return;
        }
        size_t outVertex = 0;
        // d3d11 doesn't have triangle fan so we need to convert it to triangle list
        for ( RwUInt32 i = 1; i < vertexCount - 1; i++ )
        {
            vertexArr[outVertex++] = verticles[0];
            vertexArr[outVertex++] = verticles[i];
            vertexArr[outVertex++] = verticles[i + 1];
        }
        vertCount = static_cast<RwUInt32>( vertexArr.size() );
        topologyPrim = rwPRIMTYPETRILIST;
        if ( !EnsureCapacity( vertCount, 0 ) )
            return;
        m_pVertexBuffer->Update( vertexArr.data(), sizeof( RwIm2DVertex )* vertCount );
    }
    // initialize primitive info
    g_pStateMgr->SetInputLayout( m_pVertexDecl->getInputLayout() );
    g_pStateMgr->SetVertexBuffer( m_pVertexBuffer->getBuffer(), sizeof( RwIm2DVertex ), 0 );
    g_pStateMgr->SetPrimitiveTopology( CD3D1XEnumParser::ConvertPrimTopology( topologyPrim ) );

    m_pVS->Set();
    m_pPS->Set();

    // draw primitive
    g_pStateMgr->FlushStates();
    pImmediateContext->Draw( vertCount, 0 );
}

void CD3D1XIm2DPipeline::DrawIndexed( RwPrimitiveType prim, RwIm2DVertex *vertices, RwUInt32 numVertices, RwImVertexIndex *indices, RwUInt32 numIndices )
{
    if ( vertices == nullptr || numVertices == 0 || indices == nullptr || numIndices == 0 )
        return;
    if ( prim == rwPRIMTYPETRIFAN && numIndices < 3 )
        return;
    RwPrimitiveType topologyPrim = prim;
    if ( numVertices > kIm2DMaxUploadVertices )
    {
        g_pDebug->printError( "Im2D indexed sprite batch dropped: its vertex upload size does not fit the upload size parameter." );
        return;
    }
    // dynamic vertex and index buffer update
    if ( prim != rwPRIMTYPETRIFAN )
    {
        if ( numIndices > kIm2DMaxUploadIndices )
        {
            g_pDebug->printError( "Im2D indexed sprite batch dropped: its index upload size does not fit the upload size parameter." );
            return;
        }
        if ( !EnsureCapacity( numVertices, numIndices ) )
            return;
        m_pVertexBuffer->Update( &vertices[0], sizeof( RwIm2DVertex )*numVertices );
        m_pIndexBuffer->Update( &indices[0], sizeof( RwImVertexIndex )*numIndices );
    }
    else
    {
        // d3d11 doesn't have triangle fan, so expand the fan into an explicit triangle list
        // instead of refusing the draw. The fan length is checked before the expansion is multiplied
        // out: comparing the product instead would let three per triangle wrap a 32 bit count, which
        // would size the vector for a short expansion while the loop below ran the full length and
        // wrote past its end.
        if ( static_cast<size_t>( numIndices - 2 ) > kIm2DMaxFanIndices )
        {
            g_pDebug->printError( "Im2D triangle fan batch dropped: its expanded index upload size does not fit the upload size parameter." );
            return;
        }
        const size_t expandedIndexCount = static_cast<size_t>( numIndices - 2 ) * 3;
        std::vector<RwImVertexIndex> expandedIndices;
        // The expansion is a transient host allocation, not a device resource, so it is taken before
        // either buffer grows. A batch that fits the bound above but cannot be expanded is dropped
        // here with both buffers untouched, instead of ending the process.
        try
        {
            expandedIndices.resize( expandedIndexCount );
        }
        catch ( ... )
        {
            g_pDebug->printError( "Im2D triangle fan batch dropped: its index expansion could not be allocated." );
            return;
        }
        if ( !EnsureCapacity( numVertices, static_cast<unsigned int>( expandedIndexCount ) ) )
            return;
        m_pVertexBuffer->Update( &vertices[0], sizeof( RwIm2DVertex )*numVertices );
        size_t outIndex = 0;
        for ( RwUInt32 i = 0; i < numIndices - 2; i++ )
        {
            expandedIndices[outIndex++] = indices[0];
            expandedIndices[outIndex++] = indices[i + 1];
            expandedIndices[outIndex++] = indices[i + 2];
        }
        m_pIndexBuffer->Update( expandedIndices.data(), sizeof( RwImVertexIndex ) * expandedIndexCount );
        numIndices = static_cast<RwUInt32>( expandedIndexCount );
        topologyPrim = rwPRIMTYPETRILIST;
    }

    // initialize primitive info
    g_pStateMgr->SetInputLayout( m_pVertexDecl->getInputLayout() );
    g_pStateMgr->SetVertexBuffer( m_pVertexBuffer->getBuffer(), sizeof( RwIm2DVertex ), 0 );
    g_pStateMgr->SetIndexBuffer( m_pIndexBuffer->getBuffer() );
    g_pStateMgr->SetPrimitiveTopology( CD3D1XEnumParser::ConvertPrimTopology( topologyPrim ) );

    m_pVS->Set();
    m_pPS->Set();
    // draw primitive
    g_pStateMgr->FlushStates();
    GET_D3D_RENDERER->DrawIndexed( numIndices, 0, 0 );
}