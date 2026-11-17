// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "stdafx.h"
#include "CRwGameHooks.h"
#include "RwRenderEngine.h"
CRwPointerTable CRwGameHooks::ms_rwPointerTableSA = {
    0x8E249C, // RenderSystem
    // Render state set/get funcs
    0x8E24A8, // SetRenderState;
    0x8E24AC, // GetRenderState;
    0x7F8580, // SetRefreshRate;
    0x7F8640, // SetVideoMode;
    // Immediate 2D pointers
    0x8E24B8, // Im2DRenderPrim;
    0x8E24BC, // Im2DRenderIndexedPrim;
    0x8E24B0, // Im2DRenderLine;
    // Immediate 3D pointers
    0x80A225, // Im3DOpen;
    0x8E297C, // Im3DSubmit;
    // Default pipelines nodes and callbacks
    0x8D633C, // AtomicAllInOneNode;
    0x8DED0C, // SkinAllInOneNode;
    0x757899, // DefaultInstanceCallback;
    // Renderware init/shutdown
    //INT_PTR m_fpInitRW;
    //INT_PTR m_fpShutdownRW;
    // Feature support checks
    0x745530, // CheckNativeTextureSupport;
    0x5D8980, // CheckEnviromentMapSupport;
    // Game specific func?
    0x747180 // InitGamma;
};
void CRwGameHooks::Patch( const CRwPointerTable& pointerTable )
{
    SetPointer( pointerTable.m_fpRenderSystem, RenderSystem );

    SetPointer( pointerTable.m_fpIm2DRenderPrim, Im2DRenderPrim );
    SetPointer( pointerTable.m_fpIm2DRenderIndexedPrim, Im2DRenderIndexedPrim );
    SetPointer( pointerTable.m_fpIm2DRenderLine, Im2DRenderLine );

    RedirectJump( pointerTable.m_fpIm3DOpen, Im3DOpen );
    SetPointer( pointerTable.m_fpIm3DSubmit, Im3DSubmit );

    RedirectJump( pointerTable.m_fpSetRefreshRate, SetRefreshRate );
    RedirectJump( pointerTable.m_fpSetVideoMode, SetVideoMode );

    RedirectJump( pointerTable.m_fpInitGamma, InitGamma );
    RedirectJump( pointerTable.m_fpCheckEnviromentMapSupport, CheckEnviromentMapSupport );
    RedirectJump( pointerTable.m_fpCheckNativeTextureSupport, CheckNativeTextureSupport );

    SetPointer( pointerTable.m_fpSetRenderState, SetRenderState );
    SetPointer( pointerTable.m_fpGetRenderState, GetRenderState );

    SetPointer( pointerTable.m_fpAtomicAllInOneNode, AtomicAllInOneNode );
    SetPointer( pointerTable.m_fpSkinAllInOneNode, SkinAllInOneNode );

    SetPointer( pointerTable.m_fpDefaultInstanceCallback, DefaultInstanceCallback );

    //RedirectJump(pointerTable.m_fpInitRW, InitRW);
    //RedirectJump(pointerTable.m_fpShutdownRW, ShutdownRW);
}

RwBool CRwGameHooks::RenderSystem( RwInt32 request, void * out, void * inOut, RwInt32 in )
{
    return g_pRwCustomEngine->EventHandlingSystem( (RwRenderSystemState)request, (int*)out, inOut, in );
}

RwBool CRwGameHooks::SetRenderState( RwRenderState state, UINT param )
{
    return g_pRwCustomEngine->RenderStateSet( state, param ); 
}

RwBool CRwGameHooks::GetRenderState( RwRenderState nState, UINT* pParam )
{
    // RenderStateGet stores a result for every state it is handed, so the only case it cannot answer is a
    // caller that passed no destination: that is refused here instead of being written through. Its own
    // result is returned as it stands.
    if ( pParam == nullptr )
        return false;
    return g_pRwCustomEngine->RenderStateGet( nState, *pParam );
}

void CRwGameHooks::SetRefreshRate( RwUInt32 refreshRate )
{
    UNREFERENCED_PARAMETER( refreshRate );
}

void CRwGameHooks::SetVideoMode( RwUInt32 videomode )
{
    g_pRwCustomEngine->UseMode( videomode );
}

RwBool CRwGameHooks::CheckNativeTextureSupport()
{
    // TODO: add proper check
    return true;
}

RwBool CRwGameHooks::CheckEnviromentMapSupport()
{
    // TODO: add proper check
    return true;
}

RwBool CRwGameHooks::Im2DRenderPrim( RwPrimitiveType primType, RwIm2DVertex *vertices, RwInt32 numVertices )
{
    return g_pRwCustomEngine->Im2DRenderPrimitive( primType, vertices, numVertices );
}

RwBool CRwGameHooks::Im2DRenderIndexedPrim( RwPrimitiveType primType, RwIm2DVertex *vertices, RwInt32 numVertices, RwImVertexIndex *indices, RwInt32 numIndices )
{
    return g_pRwCustomEngine->Im2DRenderIndexedPrimitive( primType, vertices, numVertices, indices, numIndices );
}

RwBool CRwGameHooks::Im2DRenderLine( RwIm2DVertex *vertices, RwInt32 numVertices, RwInt32 vert1, RwInt32 vert2 )
{
    // A line is a two-index line list, so it routes through the indexed path the engine already implements
    // rather than through a separate line pipeline. Both endpoints are caller-supplied ordinals, so they are
    // range-checked here: the indexed path trusts its indices and would read past the vertex array otherwise.
    if ( vertices == nullptr || numVertices <= 0 )
        return false;
    // RwImVertexIndex is RwUInt16 and the indexed path uploads it straight into an R16_UINT buffer, so an
    // ordinal above that range narrows to a different vertex and draws the line somewhere else instead of
    // failing. numVertices reaches the tens of millions, so it cannot stand in as the bound for that cast.
    // The parentheses around max keep the windows.h macro of the same name from expanding it, the same form
    // the Im2D and Im3D upload bounds use.
    const RwInt32 maxIndexValue = static_cast<RwInt32>( (std::numeric_limits<RwImVertexIndex>::max )() );
    if ( vert1 < 0 || vert1 >= numVertices || vert1 > maxIndexValue ||
         vert2 < 0 || vert2 >= numVertices || vert2 > maxIndexValue )
        return false;

    RwImVertexIndex indices[2] = { (RwImVertexIndex)vert1, (RwImVertexIndex)vert2 };
    // The indexed path uploads numVertices vertices and grows the shared Im2D vertex buffer to that count, so
    // handing it the caller's whole batch uploads every vertex and keeps the buffer that large for a draw that
    // reads two entries. Only the vertices the two indices can reach are needed, and the index bound above
    // already keeps that within 65536.
    const RwInt32 usedVertices = ( vert1 > vert2 ? vert1 : vert2 ) + 1;
    return g_pRwCustomEngine->Im2DRenderIndexedPrimitive( rwPRIMTYPELINELIST, vertices, usedVertices, indices, 2 );
}

void CRwGameHooks::Im3DOpen()
{
}

void __fastcall CRwGameHooks::InitGamma( void *pGamma, void * )
{
    // The game's CGamma constructor writes the object's four fields, and the game's own gamma function reads the
    // first of them as an on switch: it applies nothing while that field is clear. A hook that returns without
    // writing them leaves the object at zero, so that switch stays clear and no display gamma is ever applied.
    // The four writes below are the constructor's own, decoded from the retail executable at 0x00747180. The two
    // level fields start at the values the game's curve code compares against to decide that no target has been
    // set yet.
    //
    // The display cap query that follows those writes is deliberately not reproduced: it takes a Direct3D
    // interface through the path this renderer replaced, and it only clears the first field again on a device
    // that reports no gamma support.
    struct CGammaFields
    {
        int   m_nEnabled;        // +0x00
        float m_fCurrentLevel;   // +0x04
        float m_fTargetLevel;    // +0x08
        float m_fPrevLevel;      // +0x0C
    };
    // The offsets are the game's, not this file's, so the compiler is made to keep them here rather than
    // trusted to. A field inserted above, or two fields swapped, changes what the writes below mean while
    // still building, and this is what stops that.
    static_assert( sizeof( CGammaFields ) == 0x10, "CGamma block size changed" );
    static_assert( offsetof( CGammaFields, m_nEnabled ) == 0x00, "CGamma on switch moved" );
    static_assert( offsetof( CGammaFields, m_fCurrentLevel ) == 0x04, "CGamma current level moved" );
    static_assert( offsetof( CGammaFields, m_fTargetLevel ) == 0x08, "CGamma target level moved" );
    static_assert( offsetof( CGammaFields, m_fPrevLevel ) == 0x0C, "CGamma previous level moved" );

    CGammaFields* pFields = static_cast<CGammaFields*>( pGamma );
    pFields->m_nEnabled = 1;                // mov dword ptr [esi], 1
    pFields->m_fCurrentLevel = 0.5f;        // mov dword ptr [esi+4], 0x3F000000
    pFields->m_fTargetLevel = -1000.0f;     // mov dword ptr [esi+8], 0xC47A0000
    pFields->m_fPrevLevel = 1000.0f;        // mov dword ptr [esi+C], 0x447A0000
}

RwBool CRwGameHooks::Im3DSubmit()
{
    return g_pRwCustomEngine->Im3DSubmitNode();
}

RwBool CRwGameHooks::AtomicAllInOneNode( RxPipelineNode *self, const RxPipelineNodeParam *params )
{
    return g_pRwCustomEngine->AtomicAllInOneNode( self, params );
}

RwBool CRwGameHooks::SkinAllInOneNode( RxPipelineNode *self, const RxPipelineNodeParam *params )
{
    return g_pRwCustomEngine->SkinAllInOneNode( self, params );
}

RwBool CRwGameHooks::DefaultInstanceCallback( void *object, RxD3D9ResEntryHeader *resEntryHeader, RwBool reinstance )
{
    return g_pRwCustomEngine->DefaultInstanceCallback( object, resEntryHeader, reinstance );
}
