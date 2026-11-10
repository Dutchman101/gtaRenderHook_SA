// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "stdafx.h"
#include "DeferredPipeline.h"
#include "CDebug.h"
#include "D3DRenderer.h"
#include "D3D1XShader.h"

CDeferredPipeline::CDeferredPipeline( const std::string &pipeName, bool useVoxelPipe ) : CShadowPipeline{ pipeName }, CVoxelPipeline{ pipeName,useVoxelPipe }
{
    std::string shaderPath = "shaders/" + m_sPipeName + ".hlsl";
    // Presets may ship their own copy of a material shader without the deferred pass, so the entry point is
    // probed rather than assumed: an entry point that is not there cannot be compiled, and every stage that
    // would have used this shader falls back to the pipeline's base pixel shader. The report names its own
    // shader because the log drops repeated text, so one shared wording would report only the first of them.
    if ( CD3D1XShader::SourceDefinesEntryPoint( shaderPath, "DeferredPS" ) )
        m_pDeferredPS = new CD3D1XPixelShader( shaderPath, "DeferredPS" );
    else
        g_pDebug->printError( "Shader " + shaderPath + " defines no DeferredPS entry point, so its deferred stage uses the pipeline's base pixel shader." );
}


CDeferredPipeline::~CDeferredPipeline()
{
    delete m_pDeferredPS;
}
