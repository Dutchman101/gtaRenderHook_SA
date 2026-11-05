// Fullscreen quad. Used for postprocessing
#include "Globals.hlsl"

//--------------------------------------------------------------------------------------
// Variables
//--------------------------------------------------------------------------------------
Texture2D txPrevFrame : register(t0);
#ifndef SAMLIN
#define SAMLIN
SamplerState samLinear : register(s0);
#endif
struct VS_QUAD_IN
{
    float4 vPosition : POSITION;
    float2 vTexCoord : TEXCOORD0;
};
struct PS_QUAD_IN
{
    float4 vPosition : SV_Position;
    float4 vTexCoord : TEXCOORD0;
};
//--------------------------------------------------------------------------------------
// Vertex Shader
//--------------------------------------------------------------------------------------
PS_QUAD_IN VS(VS_QUAD_IN i)
{
    PS_QUAD_IN o;
    o.vPosition = i.vPosition;
    // the quad sits at the far plane: 0 in a reversed Z depth buffer
    if ( fReversedZ > 0 )
        o.vPosition.z = o.vPosition.w - o.vPosition.z;
    o.vTexCoord = float4(i.vTexCoord, 0, 1);
    return o;
}

float4 BlitPS(PS_QUAD_IN i) : SV_Target
{
    return txPrevFrame.Sample(samLinear, i.vTexCoord.xy);
}

// Fills a depth-only target with the quad's own depth. BlitPS cannot do this: it samples txPrevFrame,
// and the depth-only pass has no render target bound, so the read would return whatever the previous
// pass left in slot t0 and the write would land nowhere. Writing the interpolated SV_Position.z keeps
// the result correct under both depth conventions, because VS has already applied the reversed-Z flip.
void DepthClearPS(PS_QUAD_IN i, out float fDepth : SV_Depth)
{
    fDepth = i.vPosition.z;
}