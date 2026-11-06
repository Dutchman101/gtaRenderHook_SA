
// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "PBSMaterial.h"
#include "CDebug.h"
#include <filesystem>

RwTexDictionary *                               CPBSMaterialMgr::materialsTXD;
std::unordered_map<std::string_view, CPBSMaterial *, CPBSMaterialNameHash, CPBSMaterialNameEqual>
    CPBSMaterialMgr::materials{};
namespace
{
    constexpr const char *kMaterialExtension = ".mat";

    // Case-insensitive suffix test using the same folding as the name lookup above, so the extension a file is
    // accepted under and the name it is later matched by can never disagree on case.
    bool HasSuffix( std::string_view text, std::string_view suffix )
    {
        return text.size() >= suffix.size() &&
               CPBSMaterialNameEqual{}( text.substr( text.size() - suffix.size() ), suffix );
    }
}

CPBSMaterial::CPBSMaterial( const std::string &fname )
    : m_sName( HasSuffix( fname, kMaterialExtension )
                   ? std::filesystem::path( fname ).stem().string()
                   : fname )
{
    if ( CPBSMaterialMgr::materialsTXD == nullptr )
        return;
    auto file = fopen(
        ( HasSuffix( fname, kMaterialExtension ) ? fname : ( std::string( "materials\\" ) + m_sName + kMaterialExtension ) ).c_str(), "rt" );
    char specFileName[80]{};
    char normalFileName[80]{};
    if ( file )
    {
        auto res = fscanf( file, "%79s\n", specFileName );
        if ( res != 1 )
        {
            fclose( file );
            return;
        }
        m_tSpecRoughness = _RwTexDictionaryFindNamedTexture(
            CPBSMaterialMgr::materialsTXD, specFileName );

        res = fscanf( file, "%79s\n", normalFileName );

        if ( res != 1 )
        {
            fclose( file );
            return;
        }
        m_tNormals = _RwTexDictionaryFindNamedTexture(
            CPBSMaterialMgr::materialsTXD, normalFileName );
        fclose( file );
    }
    // m_tSpecRoughness
}

void CPBSMaterialMgr::LoadMaterials()
{
    for ( auto &entry : materials )
        delete entry.second;
    materials.clear();
    materialsTXD = nullptr;
    std::string     path( "materials/" );
    std::error_code ec;
    if ( !std::filesystem::exists( path, ec ) )
    {
        g_pDebug->printMsg(
            "Materials folder not found, material rendering will be disabled.",
            1 );
        return;
    }
    
    if ( !std::filesystem::exists( "materials\\materials.txd", ec ) )
    {
        g_pDebug->printMsg(
            "Materials TXD not found, material rendering will be disabled.",
            1 );
        return;
    }
    materialsTXD = CFileLoader::LoadTexDictionary( "materials\\materials.txd" );
    if ( materialsTXD == nullptr )
    {
        g_pDebug->printMsg(
            "Failed to load materials.txd, material rendering will be disabled.",
            1 );
        return;
    }
    auto dir_iter = std::filesystem::recursive_directory_iterator( path, ec );
    for ( auto &p : dir_iter )
    {
        if ( HasSuffix( p.path().extension().string(), kMaterialExtension ) )
        {
            auto materialptr = new CPBSMaterial( p.path().string() );
            if ( materialptr->m_tSpecRoughness != nullptr &&
                 materialptr->m_tSpecRoughness->raster != nullptr )
            {
                const std::string_view name( materialptr->m_sName );
                auto it = materials.find( name );
                if ( it != materials.end() )
                {
                    // The stored key is a view into the material being replaced, so the entry has to go before the
                    // delete or the map would be left holding a key into freed storage.
                    CPBSMaterial *duplicate = it->second;
                    materials.erase( it );
                    delete duplicate;
                }
                materials.emplace( name, materialptr );
            }
            else
            {
                delete materialptr;
            }
        }
    }
}

void CPBSMaterialMgr::SetMaterial( const char *textureName ) {}

CPBSMaterial *CPBSMaterialMgr::FindMaterial( const char *textureName )
{
    if ( textureName == nullptr || textureName[0] == '\0' || materials.empty() )
        return nullptr;
    auto it = materials.find( std::string_view( textureName ) );
    return ( it != materials.end() ) ? it->second : nullptr;
}
