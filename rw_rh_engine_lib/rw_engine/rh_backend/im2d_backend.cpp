#include "im2d_backend.h"
#include "../system_funcs/rw_device_system_globals.h"
#include "common_headers.h"
#include "ipc/MemoryWriter.h"
#include "raster_backend.h"
#include <Engine/Common/IBuffer.h>
#include <Engine/Common/IDeviceState.h>
#include <array>
#include <render_client/render_client.h>
#include <vector>

namespace rh::rw::engine
{

RwIm2DVertex *TriFanToTriList( RwIm2DVertex *vertices_in,
                               RwIm2DVertex *vertices_out,
                               int32_t &     vertex_count )
{
    uint32_t k = 0;
    for ( int32_t i = 1; i < vertex_count - 1; i++ )
    {
        vertices_out[k++] = ( vertices_in[0] );
        vertices_out[k++] = ( vertices_in[i] );
        vertices_out[k++] = ( vertices_in[i + 1] );
    }
    vertex_count = k;
    return vertices_out;
}

int32_t Im2DRenderPrimitiveFunction( int32_t primType, RwIm2DVertex *vertices,
                                     int32_t numVertices )
{
    if ( gRwDeviceGlobals.DeviceGlobalsPtr == nullptr ||
         gRwDeviceGlobals.DeviceGlobalsPtr->curCamera == nullptr ||
         vertices == nullptr || numVertices <= 0 )
        return 1;
    auto  to_vertices = vertices;
    auto  vert_count  = numVertices;
    auto &im2d        = gRenderClient->RenderState.Im2D;
    if ( primType == RwPrimitiveType::rwPRIMTYPETRIFAN )
    {
        if ( numVertices < 3 )
            return 1;
        std::vector<RwIm2DVertex> vertices_2;
        vertices_2.resize( static_cast<size_t>( numVertices - 2 ) * 3 );

        // convert trifan to trilist
        to_vertices =
            TriFanToTriList( vertices, vertices_2.data(), vert_count );

        im2d.RecordDrawCall( to_vertices, vert_count );
    }
    else
        im2d.RecordDrawCall( to_vertices, vert_count );
    return 1;
}

int32_t Im2DRenderIndexedPrimitiveFunction( int32_t       primType,
                                            RwIm2DVertex *vertices,
                                            int32_t       numVertices,
                                            int16_t *     indices,
                                            int32_t       numIndices )
{
    if ( gRwDeviceGlobals.DeviceGlobalsPtr == nullptr ||
         gRwDeviceGlobals.DeviceGlobalsPtr->curCamera == nullptr ||
         vertices == nullptr || numVertices <= 0 || indices == nullptr ||
         numIndices <= 0 )
        return 1;
    auto &im2d = gRenderClient->RenderState.Im2D;
    if ( primType == RwPrimitiveType::rwPRIMTYPETRIFAN )
    {
        if ( numIndices < 3 )
            return 1;
        std::vector<int16_t> tri_indices;
        tri_indices.reserve( static_cast<size_t>( numIndices - 2 ) * 3 );
        for ( int32_t i = 1; i < numIndices - 1; ++i )
        {
            tri_indices.push_back( indices[0] );
            tri_indices.push_back( indices[i] );
            tri_indices.push_back( indices[i + 1] );
        }
        im2d.RecordDrawCall( vertices, numVertices, tri_indices.data(),
                             static_cast<int32_t>( tri_indices.size() ) );
    }
    else
        im2d.RecordDrawCall( vertices, numVertices, indices, numIndices );
    return 1;
}
} // namespace rh::rw::engine