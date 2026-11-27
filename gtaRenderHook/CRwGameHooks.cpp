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
    // A site already holding this plugin's value is left alone; a site holding another module's value is named and then
    // taken, because every entry below is a way the renderer reaches its own code.
    SetPointerExclusive( "RW: render system", pointerTable.m_fpRenderSystem, reinterpret_cast<void *>( &RenderSystem ) );

    SetPointerExclusive( "RW: immediate 2D render primitive", pointerTable.m_fpIm2DRenderPrim,
                         reinterpret_cast<void *>( &Im2DRenderPrim ) );
    SetPointerExclusive( "RW: immediate 2D indexed primitive", pointerTable.m_fpIm2DRenderIndexedPrim,
                         reinterpret_cast<void *>( &Im2DRenderIndexedPrim ) );
    SetPointerExclusive( "RW: immediate 2D line", pointerTable.m_fpIm2DRenderLine,
                         reinterpret_cast<void *>( &Im2DRenderLine ) );

    RedirectJump( pointerTable.m_fpIm3DOpen, Im3DOpen );
    SetPointerExclusive( "RW: immediate 3D submit", pointerTable.m_fpIm3DSubmit,
                         reinterpret_cast<void *>( &Im3DSubmit ) );

    RedirectJump( pointerTable.m_fpSetRefreshRate, SetRefreshRate );
    RedirectJump( pointerTable.m_fpSetVideoMode, SetVideoMode );

    RedirectJump( pointerTable.m_fpInitGamma, InitGamma );
    RedirectJump( pointerTable.m_fpCheckEnviromentMapSupport, CheckEnviromentMapSupport );
    RedirectJump( pointerTable.m_fpCheckNativeTextureSupport, CheckNativeTextureSupport );

    SetPointerExclusive( "RW: set render state", pointerTable.m_fpSetRenderState,
                         reinterpret_cast<void *>( &SetRenderState ) );
    SetPointerExclusive( "RW: get render state", pointerTable.m_fpGetRenderState,
                         reinterpret_cast<void *>( &GetRenderState ) );

    // The two atomic callbacks the whole frame's geometry passes through, so a value that does not reach this
    // renderer here is a world that does not draw while everything around it still runs.
    SetPointerExclusive( "RW: atomic all in one node", pointerTable.m_fpAtomicAllInOneNode,
                         reinterpret_cast<void *>( &AtomicAllInOneNode ) );
    SetPointerExclusive( "RW: skin all in one node", pointerTable.m_fpSkinAllInOneNode,
                         reinterpret_cast<void *>( &SkinAllInOneNode ) );

    // This one entry is not a value on its own in the executable's data: it is the four byte operand of a
    // mov dword ptr [eax], imm32 at 0x00757899, so it carries the two bytes in front of it.
    SetPointerExclusive( "RW: default instance callback", pointerTable.m_fpDefaultInstanceCallback,
                         reinterpret_cast<void *>( &DefaultInstanceCallback ), SITE_PREFIX_MOV_EAX_IMM32 );

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
    // RenderStateGet stores a result for every state it is handed, so the only case it cant answer is a caller that
    // passed no destination. That is refused here instead of being written through.
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
    // A line is a two-index line list, so it routes through the indexed path rather than a separate line pipeline. Both
    // endpoints are caller-supplied ordinals, so they are range-checked here: the indexed path trusts its indices.
    if ( vertices == nullptr || numVertices <= 0 )
        return false;
    // RwImVertexIndex is RwUInt16 and the indexed path uploads it into an R16_UINT buffer, so a higher ordinal narrows to a
    // different vertex, and numVertices reaches the tens of millions so it cant bound that cast. The parentheses keep the
    // windows.h max macro from expanding it.
    const RwInt32 maxIndexValue = static_cast<RwInt32>( (std::numeric_limits<RwImVertexIndex>::max )() );
    if ( vert1 < 0 || vert1 >= numVertices || vert1 > maxIndexValue ||
         vert2 < 0 || vert2 >= numVertices || vert2 > maxIndexValue )
        return false;

    RwImVertexIndex indices[2] = { (RwImVertexIndex)vert1, (RwImVertexIndex)vert2 };
    // The indexed path uploads numVertices vertices and grows the shared Im2D buffer to that count, so handing it the whole
    // batch keeps it that large for a draw reading two entries. Only what the two indices reach is needed.
    const RwInt32 usedVertices = ( vert1 > vert2 ? vert1 : vert2 ) + 1;
    return g_pRwCustomEngine->Im2DRenderIndexedPrimitive( rwPRIMTYPELINELIST, vertices, usedVertices, indices, 2 );
}

void CRwGameHooks::Im3DOpen()
{
}

void __fastcall CRwGameHooks::InitGamma( void *pGamma, void * )
{
    // The game's CGamma constructor writes the object's four fields, and its gamma function reads the first as an on switch
    // and applies nothing while that is clear, so a hook that returns without writing them leaves no display gamma ever
    // applied. The four writes below are the constructor's own, decoded at 0x00747180. The display cap query after them is
    // deliberately not reproduced: it reaches Direct3D through the path this renderer replaced.
    struct CGammaFields
    {
        int   m_nEnabled;        // +0x00
        float m_fCurrentLevel;   // +0x04
        float m_fTargetLevel;    // +0x08
        float m_fPrevLevel;      // +0x0C
    };
    // The offsets are the game's, not this file's, so the compiler is made to keep them: a field inserted above, or two
    // swapped, changes what the writes below mean while still building.
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
