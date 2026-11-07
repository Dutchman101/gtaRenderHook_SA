#include "GameMath.hlsl"
#include "Shadows.hlsl"
#include "LightingFunctions.hlsl"
#include "ReflectionFunctions.hlsli"
#include "GBuffer.hlsl"
#include "VoxelGI.hlsl"
#include "Shadows.hlsl"
#include "AtmosphericScatteringFunctions.hlsli"
//--------------------------------------------------------------------------------------
// Variables
//--------------------------------------------------------------------------------------
Texture2D txGB0 	: register(t0);
Texture2D txGB1 	: register(t1);
Texture2D txGB2 	: register(t2);
Texture2D txPrevFrame : register(t3);
TextureCube txCubeMap : register(t5);
#ifndef SAMLIN
#define SAMLIN
SamplerState samLinear : register(s0);
#endif
struct PSInput_Quad
{
    float4 pos : SV_Position;
    float4 texCoordOut : TEXCOORD0;
};

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 ReflectionPassPS(PSInput_Quad input) : SV_Target
{
    float4 NormalSpec = txGB1.Sample(samLinear, input.texCoordOut.xy);
    float ViewZ = DecodeFloatRG(NormalSpec.zw);
    ViewZ = ViewZ <= 0 ? fFarClip : ViewZ;
    float3 cameraSpacePos = DepthToViewPos(ViewZ, input.texCoordOut.xy).xyz;
    float3 worldSpacePos = SSRDepthToWorldPos(ViewZ, input.texCoordOut.xy);
    float3 NormalsVS = DecodeNormals(NormalSpec.xy);
    NormalsVS = mul(float4(NormalsVS, 0.0f), mViewInv).rgb;
    float2 Parameters = txGB2.Sample(samLinear, input.texCoordOut.xy).xy;
    float Roughness = 1 - Parameters.y;

    const float3 ViewPos = mViewInv[3].xyz;

    float3 ViewDir = normalize(worldSpacePos - ViewPos);
    float3 ReflDir = normalize(reflect(ViewDir, NormalsVS));
    
    // simply return SSR for now, maybe will use DXR someday, but simple cubemap is way more possible 
    float Fallback=0;
    // Glossy jitter is off, so ReflDir reaches the SSR trace and the cubemap fallback as one unchanged
    // vector. Turning jitter back on means perturbing ReflDir once, before the trace, and using that same
    // perturbed vector for both, otherwise the two reflections disagree.
    float3 SSRColor = SSR(txPrevFrame, txGB1, samLinear, worldSpacePos, ReflDir, Roughness, Fallback);
    
    float3 LightDir = normalize(vSunLightDir.xyz);
    float3 FullScattering;
    // Only the FullScattering out parameter is wanted here; the returned fog colour is not used.
    CalculateFogColor(float3(0,0,0), ReflDir, LightDir, 1000, 0, FullScattering);
    
    float3 SkyColor = GetSkyColor(ReflDir, LightDir, FullScattering);
    float3 ReflectionFallBack;
#if SAMPLE_CUBEMAP ==1
    ReflDir.x *= -1;
    float4 CubeMap = txCubeMap.SampleLevel(samLinear, ReflDir, Roughness*9.0f);
    ReflectionFallBack = lerp(CubeMap.rgb, SkyColor, 1 - CubeMap.a);
#else
    ReflectionFallBack = SkyColor;
#endif
    float3 FinalReflect;
#if USE_SSR==1
    FinalReflect = lerp(SSRColor, ReflectionFallBack, Fallback);
#else
    FinalReflect = ReflectionFallBack;
#endif
    return float4(isnan(FinalReflect) ? 0.0f.xxx: FinalReflect, 1);
}
