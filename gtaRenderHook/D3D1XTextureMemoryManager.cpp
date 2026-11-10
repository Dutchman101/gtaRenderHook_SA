// This is an open source non-commercial project. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com

#include "stdafx.h"
#include "D3D1XTextureMemoryManager.h"
#include "D3D1XBaseTexture.h"

std::list<CD3D1XBaseTexture*> CD3D1XTextureMemoryManager::textureList = {};

void CD3D1XTextureMemoryManager::AddNew( CD3D1XBaseTexture* &tex )
{
    textureList.push_back( tex );
}

void CD3D1XTextureMemoryManager::Remove( CD3D1XBaseTexture * &tex )
{
    textureList.remove( tex );
    delete tex;
    // Only the name the caller passed in is cleared, because that is the pointer the raster keeps. A slot left naming
    // released memory is read by the raster lock path, which tests it for null before it uses it.
    tex = nullptr;
}

void CD3D1XTextureMemoryManager::Shutdown()
{
    for ( auto &tex : textureList )
    {
        // GetD3D1XRaster is an offset into the raster it is given, so an absent parent has to be ruled out before it is
        // used to reach the field: there is no null to compare against once the offset has been added.
        RwRaster* pParent = tex ? tex->GetParent() : nullptr;
        if ( pParent )
        {
            auto d3dRaster = GetD3D1XRaster( pParent );
            if ( d3dRaster->resourse == tex )
                d3dRaster->resourse = nullptr;
        }
        delete tex;
    }
    // The list outlives this call, so what it still holds names textures that no longer exist: a later release frees
    // them a second time, and a later removal matches against addresses a new texture can reuse.
    textureList.clear();
}
