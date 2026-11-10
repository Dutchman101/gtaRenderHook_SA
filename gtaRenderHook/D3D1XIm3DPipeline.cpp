// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "D3D1XIm3DPipeline.h"
#include "CDebug.h"
#include "D3DRenderer.h"
#include "D3D1XShader.h"
#include "D3D1XStateManager.h"
#include "RwD3D1XEngine.h"
#include "D3D1XEnumParser.h"
#include "DeferredRenderer.h"

// Update takes the upload size as an int, so a batch whose byte count does not fit that parameter
// cannot be uploaded at all. Dividing the limit by the element size yields the largest count that
// fits, and the division itself cannot wrap.
static const size_t kIm3DMaxUploadVertices = static_cast<size_t>( (std::numeric_limits<int>::max )() ) / sizeof( RwIm3DVertex );
static const size_t kIm3DMaxUploadIndices = static_cast<size_t>( (std::numeric_limits<int>::max )() ) / sizeof( RwImVertexIndex );

CD3D1XIm3DPipeline::CD3D1XIm3DPipeline() :
#ifndef DebuggingShaders
    CD3D1XPipeline( "RwIm3D" )
#else
    CD3D1XPipeline( L"RwIm3D" )
#endif // !DebuggingShaders
{
    std::vector<D3D11_INPUT_ELEMENT_DESC> layout =
    {
        { "POSITION",	0, DXGI_FORMAT_R32G32B32_FLOAT,		0, 0,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL",		0, DXGI_FORMAT_R32G32B32_FLOAT,		0, 12,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",		0, DXGI_FORMAT_R8G8B8A8_UNORM,		0, 24,	D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD",	0, DXGI_FORMAT_R32G32_FLOAT,		0, 28,	D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };
    m_pVertexDeclaration = new CD3D1XVertexDeclaration( layout, sizeof( RwIm3DVertex ), m_pVS );
    m_pVertexBuffer = new CD3D1XDynamicVertexBuffer( sizeof( RwIm3DVertex ), m_uiVertexCapacity );
    m_pIndexBuffer = new CD3D1XDynamicIndexBuffer( m_uiIndexCapacity );
    m_pVertexBuffer->SetDebugName( "Im3D_DynamicVB" );
    m_pIndexBuffer->SetDebugName( "Im3D_DynamicIB" );
    const std::string shaderPath = "shaders/" + m_sPipeName + ".hlsl";
    // Presets may ship their own copy of the immediate-mode shader without the deferred pass; a missing
    // entry point cannot be compiled, so that stage falls back to the pipeline's base pixel shader.
    if ( CD3D1XShader::SourceDefinesEntryPoint( shaderPath, "DeferredPS" ) )
        m_pDeferredPS = new CD3D1XPixelShader( shaderPath, "DeferredPS" );
}

// Plugins draw more than the initial 10000 vertices (e.g. the LOD lights of Project2DFX), so the buffers
// grow to fit the batch instead of the upload being written past their end.
// Returns whether the buffers now cover the request. A growth that fails leaves the old buffer in place,
// and the upload clamps itself to it while the draw call keeps using the requested count, so the caller has
// to drop the batch when this reports failure.
bool CD3D1XIm3DPipeline::EnsureCapacity( unsigned int vertexCount, unsigned int indexCount )
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
            grownBuffer = new CD3D1XDynamicVertexBuffer( sizeof( RwIm3DVertex ), grownCapacity );
        }
        catch ( ... )
        {
            g_pDebug->printError( "Im3D vertex batch dropped: the vertex buffer could not grow to fit it." );
            return false;
        }
        // The state manager caches the bound buffer by pointer. Releasing the old buffer without
        // clearing that slot lets a recycled address compare equal and skip the rebind, leaving a
        // released ID3D11Buffer bound for the next draw.
        g_pStateMgr->SetVertexBuffer( nullptr, 0, 0 );
        delete m_pVertexBuffer;
        m_pVertexBuffer = grownBuffer;
        m_uiVertexCapacity = grownCapacity;
        m_pVertexBuffer->SetDebugName( "Im3D_DynamicVB" );
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
            g_pDebug->printError( "Im3D index batch dropped: the index buffer could not grow to fit it." );
            return false;
        }
        // SetIndexBuffer compares the same cached pointer, so the slot is cleared for the same reason.
        g_pStateMgr->SetIndexBuffer( nullptr );
        delete m_pIndexBuffer;
        m_pIndexBuffer = grownBuffer;
        m_uiIndexCapacity = grownCapacity;
        m_pIndexBuffer->SetDebugName( "Im3D_DynamicIB" );
    }
    return true;
}


CD3D1XIm3DPipeline::~CD3D1XIm3DPipeline()
{
    delete m_pVertexDeclaration;
    delete m_pIndexBuffer;
    delete m_pVertexBuffer;
    delete m_pDeferredPS;
}

RwBool CD3D1XIm3DPipeline::SubmitNode()
{
    if ( rwD3D9ImmPool == nullptr || rwD3D9ImmPool->elements == nullptr || rwD3D9ImmPool->numElements <= 0 )
        return true;
    ID3D11DeviceContext* pImmediateContext = GET_D3D_CONTEXT;
    //rwIm3DPool* pool = rwD3D9ImmPool;
    auto im3dPoolStash = rwD3D9ImmPool->stash;

    if ( im3dPoolStash.primType != rwPRIMTYPETRIFAN )
    {
        // RenderWare hands over signed counts, and widening a negative one would size both the buffer
        // request and the upload at roughly 4 G elements, so the counts are validated here once and
        // every later use, including the two upload sizes and both draw calls, reads the result.
        const unsigned int vertexCount = static_cast<unsigned int>( rwD3D9ImmPool->numElements );
        const unsigned int indexCount = im3dPoolStash.indices != nullptr && im3dPoolStash.numIndices > 0
            ? static_cast<unsigned int>( im3dPoolStash.numIndices ) : 0u;
        if ( vertexCount > kIm3DMaxUploadVertices || indexCount > kIm3DMaxUploadIndices )
        {
            g_pDebug->printError( "Im3D batch dropped: its upload size does not fit the upload size parameter." );
            return true;
        }
        const bool hasIndices = indexCount != 0;
        if ( !EnsureCapacity( vertexCount, indexCount ) )
            return true;
        // update verticles
        m_pVertexBuffer->Update( &( (RwIm3DVertex*)rwD3D9ImmPool->elements )[0], sizeof( RwIm3DVertex )*vertexCount );
        // initialize vertex declaration and buffer
        g_pStateMgr->SetInputLayout( m_pVertexDeclaration->getInputLayout() );
        g_pStateMgr->SetVertexBuffer( m_pVertexBuffer->getBuffer(), sizeof( RwIm3DVertex ), 0 );
        // initialize primitive topology
        g_pStateMgr->SetPrimitiveTopology( CD3D1XEnumParser::ConvertPrimTopology( im3dPoolStash.primType ) );
        // initialize shaders
        m_pVS->Set();
        // The game draws smashed objects (e.g. a knocked-over lamppost) in immediate mode from the G-buffer pass. There every
        // G-buffer target has to be written and nothing blended, or the pieces get the normals and depth of what is behind.
        // The disable covers the whole stage, not just the branch that binds the deferred shader: a shader without that
        // entry point falls back to the base one and still writes the G-buffer pass, so it needs the same disable.
        const BOOL oldBlendState = g_pStateMgr->GetAlphaBlendEnable();
        if ( m_uiDeferredStage == 1 )
        {
            if ( m_pDeferredPS && m_pDeferredPS->IsValid() )
                m_pDeferredPS->Set();
            else
                m_pPS->Set();
            g_pStateMgr->SetAlphaBlendEnable( FALSE );
        }
        else
            m_pPS->Set();
        // draw mesh
        if ( hasIndices )
        {
            // update index buffer
            m_pIndexBuffer->Update( &im3dPoolStash.indices[0], sizeof( RwImVertexIndex )*indexCount );
            // initialize vertex buffer
            g_pStateMgr->SetIndexBuffer( m_pIndexBuffer->getBuffer() );

            g_pStateMgr->FlushStates();
            pImmediateContext->DrawIndexed( indexCount, 0, 0 );
            g_pStateMgr->SetAlphaBlendEnable( oldBlendState );
        }
        else
        {
            g_pStateMgr->FlushStates();
            pImmediateContext->Draw( vertexCount, 0 );
            g_pStateMgr->SetAlphaBlendEnable( oldBlendState );
        }
    }
    else
    {
        // TODO: perhaps add triangle fan rendering
        g_pDebug->printMsg( "D3D1XIm3DPipeline: Triangle fan primitive is currently unsuported.", 0 );
    }
    return true;
}
