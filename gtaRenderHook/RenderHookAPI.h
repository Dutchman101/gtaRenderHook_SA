// Exported C API for external ASI plugins (lookup via GetProcAddress on the loaded RenderHook module;
// a missing export indicates an older build without that hook).
#pragma once
#include <d3d11.h>

/*!
    Per-frame inputs for an external temporal anti-aliasing replacement (invoked after deferred and forward
    passes, prior to tonemapping).
*/
struct RHTemporalAAFrame
{
    unsigned int              size; // sizeof( RHTemporalAAFrame ) for ABI extension checks
    ID3D11DeviceContext *     context; // RenderHook immediate device context
    ID3D11Texture2D *         input; // jittered HDR scene color for the current frame
    ID3D11Texture2D *         output; // destination anti-aliased HDR texture (matches input size and format)
    ID3D11ShaderResourceView *gbuffer1; // G-buffer 1: encoded view-space normals (xy) and view-space depth (zw)
    float                     jitterX; // NDC projection offset applied to _31 (x) and _32 (y) this frame
    float                     jitterY;
    float                     view[16]; // current view matrix (row vectors, matching RwD3D9D3D9ViewTransform)
    float                     projection[16]; // current unjittered projection matrix (matching RwD3D9D3D9ProjTransform)
    // Valid when size >= offsetof( RHTemporalAAFrame, objectMotion ) + sizeof( objectMotion ).
    // RGBA16F SRV (or nullptr): xy = unjittered pixel displacement to the previous frame,
    // z = 1 where a dynamic object wrote its own motion and 0 where camera depth reprojection applies.
    ID3D11ShaderResourceView *objectMotion;
};

/*!
    Invoked once per frame in place of built-in temporal AA. Returning true indicates output was written;
    returning false falls back to built-in TAA for the frame.
*/
typedef bool( __cdecl *RHTemporalAAHandler )( const RHTemporalAAFrame *frame );

/*!
    Registers an external handler (such as DLAA) to replace built-in temporal AA, or clears it when nullptr.
    Keep EnableTAA active in the TAA settings block so projection jitter and TAA pass dispatch remain active.
*/
#ifdef GTARENDERHOOK_EXPORTS
#define RENDERHOOK_API extern "C" __declspec( dllexport )
#else
#define RENDERHOOK_API extern "C" __declspec( dllimport )
#endif

RENDERHOOK_API void __cdecl RHSetTemporalAAHandler( RHTemporalAAHandler handler );

/*!
    Active rendering stage of a building mesh draw (RHBuildingDraw::pass).
*/
enum RHDrawPass
{
    RH_PASS_FORWARD = 0, // forward shading (including cubemap reflections)
    RH_PASS_GBUFFER = 1, // deferred G-buffer fill
    RH_PASS_SHADOW  = 2, // directional sun shadow cascade
    RH_PASS_BLENDED = 3, // post-deferred alpha-blended queue
    RH_PASS_OTHER   = 4 // auxiliary stages such as voxelization
};

/*!
    Descriptor for a single building mesh draw or alpha-queue submission in CustomBuilding / CustomBuildingDN.
*/
struct RHBuildingDraw
{
    unsigned int         size; // sizeof( RHBuildingDraw ) for ABI extension checks
    ID3D11DeviceContext *context; // immediate context for binding custom per-draw constant buffers
    void *               atomic; // owning RpAtomic *
    const float *        worldMatrix; // 16-float RwMatrix LTM (translation at [12], [13], [14])
    int                  meshIndex; // mesh index within the atomic geometry
    int                  pass; // RHDrawPass value
    int                  dualPassEdges; // 1 during the soft-edge blended draw of a dual-pass mesh
};

/*!
    Bitmask flags returned by RHBuildingDrawCallback.
    RH_DRAW_VERTEX_ALPHA_IS_NOT_OPACITY indicates prelight vertex alpha stores non-opacity data (such as
    vegetation wind weights) so vertex alpha does not force alpha blending or disable dual-pass cutout.
*/
const unsigned int RH_DRAW_VERTEX_ALPHA_IS_NOT_OPACITY = 1;

typedef unsigned int( __cdecl *RHBuildingDrawCallback )( const RHBuildingDraw *draw );

/*!
    Registers a per-mesh building draw callback across all passes, or clears it when nullptr.
    Custom constant buffers bound on slots outside RenderHook's b0-b11 range remain bound for that draw.
*/
RENDERHOOK_API void __cdecl RHSetBuildingDrawCallback( RHBuildingDrawCallback callback );
