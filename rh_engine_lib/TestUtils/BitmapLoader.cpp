//
// Created by peter on 02.08.2020.
//

#include "BitmapLoader.h"
#include "DebugUtils/DebugLogger.h"
#include "common.h"
#include <Engine/Common/IDeviceState.h>
#include <cassert>
#include <filesystem>

void rh::tests::LoadBMPImage( std::string_view       path,
                              engine::IDeviceState & device,
                              engine::IImageBuffer **res_buffer )
{
    assert( res_buffer != nullptr );
    *res_buffer = nullptr;

    auto fspath = std::filesystem::path( path );
    if ( !std::filesystem::exists( fspath ) && fspath.is_relative() )
    {
        std::string module_path( 256, '\0' );
        DWORD       module_len = 0;
        while ( module_path.size() <= 32768 )
        {
            module_len = GetModuleFileNameA(
                nullptr, module_path.data(),
                static_cast<DWORD>( module_path.size() ) );
            if ( module_len == 0 || module_len < module_path.size() )
                break;
            module_path.resize( module_path.size() * 2, '\0' );
        }
        if ( module_len > 0 && module_len < module_path.size() )
        {
            module_path.resize( module_len );
            const auto candidate =
                std::filesystem::path( module_path ).parent_path() / fspath;
            if ( std::filesystem::exists( candidate ) )
                fspath = candidate;
        }
    }

    if ( !std::filesystem::exists( fspath ) )
    {
        std::stringstream ss;
        ss << "Trying to open file, that doesn't exist:" << fspath;
        debug::DebugLogger::Error( ss.str() );
        return;
    }

    std::ifstream file( fspath, std::ios::binary | std::ios::in );
    if ( !file.is_open() )
    {
        debug::DebugLogger::Error( "Failed to open BMP file: " +
                                   fspath.string() );
        return;
    }

    BITMAPFILEHEADER header{};
    BITMAPINFOHEADER info_header{};

    file.read( reinterpret_cast<char *>( &header ), sizeof( header ) );
    file.read( reinterpret_cast<char *>( &info_header ),
               sizeof( info_header ) );
    if ( !file || header.bfType != 0x4D42 || info_header.biWidth <= 0 ||
         info_header.biHeight == 0 ||
         ( info_header.biBitCount != 24 && info_header.biBitCount != 32 ) )
    {
        debug::DebugLogger::Error( "Invalid or unsupported BMP header: " +
                                   fspath.string() );
        return;
    }

    const uint32_t width  = static_cast<uint32_t>( info_header.biWidth );
    const uint32_t height = static_cast<uint32_t>(
        info_header.biHeight < 0 ? -info_header.biHeight
                                 : info_header.biHeight );
    const uint32_t bytes_per_pixel =
        static_cast<uint32_t>( info_header.biBitCount / 8 );
    const uint64_t row_raw_bytes =
        static_cast<uint64_t>( width ) * bytes_per_pixel;
    const uint64_t row_stride = ( row_raw_bytes + 3ULL ) & ~3ULL;

    file.seekg( 0, std::ios::end );
    const auto file_end = file.tellg();
    if ( file_end <= static_cast<std::streamoff>( header.bfOffBits ) )
    {
        debug::DebugLogger::Error( "BMP pixel offset out of range: " +
                                   fspath.string() );
        return;
    }
    const uint64_t available_bytes = static_cast<uint64_t>(
        file_end - static_cast<std::streamoff>( header.bfOffBits ) );
    const uint64_t tight_size = row_raw_bytes * height;
    if ( available_bytes < tight_size )
    {
        debug::DebugLogger::Error( "BMP pixel payload is truncated: " +
                                   fspath.string() );
        return;
    }
    const uint64_t read_stride =
        ( available_bytes >= row_stride * height ) ? row_stride : row_raw_bytes;
    const uint64_t read_size = read_stride * height;

    file.seekg( header.bfOffBits, std::ios::beg );

    std::string texture_buff( static_cast<size_t>( read_size ), 0 );
    file.read( texture_buff.data(), static_cast<std::streamsize>( read_size ) );
    if ( !file )
    {
        debug::DebugLogger::Error( "Failed reading BMP pixels: " +
                                   fspath.string() );
        return;
    }

    // convert to rgba
    std::string texture_buff_rgba( static_cast<size_t>( width ) * height * 4u,
                                   0 );

    for ( uint32_t y = 0; y < height; ++y )
    {
        const char *src_row = texture_buff.data() + y * read_stride;
        char       *dst_row =
            texture_buff_rgba.data() + static_cast<size_t>( y ) * width * 4u;
        for ( uint32_t x = 0; x < width; ++x )
        {
            dst_row[x * 4]     = src_row[x * bytes_per_pixel];
            dst_row[x * 4 + 1] = src_row[x * bytes_per_pixel + 1];
            dst_row[x * 4 + 2] = src_row[x * bytes_per_pixel + 2];
            dst_row[x * 4 + 3] =
                ( bytes_per_pixel == 4 ) ? src_row[x * 4 + 3] : 0xF;
        }
    }

    engine::ImageBufferCreateParams image_buffer_ci{
        .mDimension = engine::ImageDimensions::d2D,
        .mFormat    = engine::ImageBufferFormat::RGBA8,
        .mHeight    = height,
        .mWidth     = width };

    std::array blue_ns_data      = { engine::ImageBufferInitData{
        texture_buff_rgba.data(),
        static_cast<uint32_t>( texture_buff_rgba.size() ),
        static_cast<uint32_t>( width * sizeof( uint32_t ) ) } };
    image_buffer_ci.mPreinitData = blue_ns_data;

    *res_buffer = device.CreateImageBuffer( image_buffer_ci );
}
