#pragma once
#include "D3D1XShader.h"
#include "D3D1XConstantBuffer.h"
struct CBVoxel
{
    RwMatrix View[6];
    float  voxelGridScale;
    float  _padding[3];
};
class CVoxelOctreeRenderer
{
public:
    // The clip map, the radiance volumes and the level of detail all span the same count: each array holds one entry
    // per level, the lod parameters index them from 1 (or from 0 for the radiance array), and the caller that walks
    // the levels takes its bound from here instead of repeating the number.
    const static int voxelLodCount = 3;
private:

    static RwTexture* voxelOctreeBricks;
    static RwTexture* voxelOctreeStruct;
    static RwRaster* voxelRadiance[voxelLodCount];

    static RwRaster* voxelClipMap[voxelLodCount];

    //static RwRaster* voxelNormals[4];
    static CD3D1XComputeShader* m_voxelCS;
    //static CD3D1XComputeShader* m_blurCS;
    static CD3D1XComputeShader* m_radianceInjectCS;
    const static UINT32 voxelTreeSize = 32;
    static CD3D1XConstantBuffer<CBVoxel>* m_pVoxelCB;
public:
    static RwCamera* m_pVoxelCamera;
    static void Init();
    static void Shutdown();
    static void CleanVoxelOctree();
    static void InjectRadiance( RwRaster* shadow, void( *emmissiveObjRender )( ), int voxelTextureID = 0 );
    static void FilterVoxelOctree();
    static void RenderToVoxelOctree( void( *render )( ), const int &lod );
    static void SetVoxelTex( RwRGBA &color );
    static void SetVoxelLODSize( const int &lod );
};
/*class VoxelOctree {
private:
    CComPtr<ID3D11Buffer>						m_VoxelDataBuffer;
    CComPtr<ID3D11Buffer>						m_VoxelLookupUAV;
    CComPtr<ID3D11UnorderedAccessView>			m_VoxelDataUAV;
    CComPtr<ID3D11UnorderedAccessView>			m_VoxelLookupUAV;
    CComPtr<ID3D11ShaderResourceView>			m_VoxelDataSRV;
    CComPtr<ID3D11ShaderResourceView>			m_VoxelLookupSRV;
public:
    VoxelOctree(ID3D11Device * pDev) {
        D3D11_BUFFER_DESC desc = {};
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;

        pDev->CreateBuffer(&desc,nullptr,&m_VoxelDataBuffer);
    }
};*/