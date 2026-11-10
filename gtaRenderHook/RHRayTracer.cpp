#include "stdafx.h"
#include "RHRayTracer.h"

CD3D1XComputeShader* RHRayTracer::ms_pRayTraceShader = nullptr;

void RHRayTracer::Init()
{
    // A second run would otherwise overwrite the handle the first one owns and leak it, so it starts from empty.
    Shutdown();
    ms_pRayTraceShader = new CD3D1XComputeShader( "RayTracing.hlsl", "main" );
}

void RHRayTracer::Render()
{
    // Start-up creates the shader and a compile that failed leaves the object in place with no shader inside it, so a
    // render that runs without a usable one has nothing to dispatch through.
    if ( ms_pRayTraceShader == nullptr || !ms_pRayTraceShader->IsValid() )
        return;
    ms_pRayTraceShader->Set();
}

void RHRayTracer::Shutdown()
{
    // Cleared as it is released, so a repeated shutdown does not release it twice and a render after this does not
    // read a released handle.
    delete ms_pRayTraceShader;
    ms_pRayTraceShader = nullptr;
}
