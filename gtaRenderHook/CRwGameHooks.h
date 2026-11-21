#pragma once

// Unsigned like MemoryFuncs.h, because a signed one cant hold a pointer at or above 0x80000000.
struct CRwPointerTable
{
    // Main render state-machine system
    UINT_PTR m_fpRenderSystem;
    // Render state set/get funcs
    UINT_PTR m_fpSetRenderState;
    UINT_PTR m_fpGetRenderState;
    UINT_PTR m_fpSetRefreshRate;
    UINT_PTR m_fpSetVideoMode;
    // Immediate 2D pointers
    UINT_PTR m_fpIm2DRenderPrim;
    UINT_PTR m_fpIm2DRenderIndexedPrim;
    UINT_PTR m_fpIm2DRenderLine;
    // Immediate 3D pointers
    UINT_PTR m_fpIm3DOpen;
    UINT_PTR m_fpIm3DSubmit;
    // Default pipelines nodes and callbacks
    UINT_PTR m_fpAtomicAllInOneNode;
    UINT_PTR m_fpSkinAllInOneNode;
    UINT_PTR m_fpDefaultInstanceCallback;
    // Renderware init/shutdown
    //INT_PTR m_fpInitRW;
    //INT_PTR m_fpShutdownRW;
    // Feature support checks
    UINT_PTR m_fpCheckNativeTextureSupport;
    UINT_PTR m_fpCheckEnviromentMapSupport;
    // Game specific func?
    UINT_PTR m_fpInitGamma;
};

class CRwGameHooks
{
public:
    static void Patch( const CRwPointerTable& pointerTable );

    static RwBool RenderSystem( RwInt32 request, void *out, void *inOut, RwInt32 in );
    static RwBool SetRenderState( RwRenderState state, UINT param );
    static RwBool GetRenderState( RwRenderState state, UINT* param );
    static void SetRefreshRate( RwUInt32 refreshRate );
    static void SetVideoMode( RwUInt32 videomode );

    static RwBool Im2DRenderPrim( RwPrimitiveType primType, RwIm2DVertex *vertices, RwInt32 numVertices );
    static RwBool Im2DRenderIndexedPrim( RwPrimitiveType primType, RwIm2DVertex *vertices, RwInt32 numVertices, RwImVertexIndex *indices, RwInt32 numIndices );
    static RwBool Im2DRenderLine( RwIm2DVertex *vertices, RwInt32 numVertices, RwInt32 vert1, RwInt32 vert2 );

    static RwBool Im3DSubmit();
    static void Im3DOpen();

    static RwBool CheckNativeTextureSupport();
    static RwBool CheckEnviromentMapSupport();

    static RwBool AtomicAllInOneNode( RxPipelineNode *self, const RxPipelineNodeParam *params );
    static RwBool SkinAllInOneNode( RxPipelineNode *self, const RxPipelineNodeParam *params );

    static RwBool DefaultInstanceCallback( void *object, RxD3D9ResEntryHeader *resEntryHeader, RwBool reinstance );

    //static void InitRW();
    //static void ShutdownRW();
    // The game reaches this as CGamma's constructor, so the object arrives in ecx and nothing is pushed.
    // Fastcall is what reads that register; the second parameter is the register fastcall reserves and
    // the game never passes, so it is left unnamed.
    static void __fastcall InitGamma( void *pGamma, void * );
    static CRwPointerTable ms_rwPointerTableSA;
};

