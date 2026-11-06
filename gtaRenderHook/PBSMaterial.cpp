
// This is an open source non-commercial project. Dear PVS-Studio, please check
// it. PVS-Studio Static Code Analyzer for C, C++ and C#: http://www.viva64.com
#include "PBSMaterial.h"
#include "CDebug.h"
#include <filesystem>

RwTexDictionary *                               CPBSMaterialMgr::materialsTXD;
std::unordered_map<std::string, CPBSMaterial *> CPBSMaterialMgr::materials{};
bool has_suffix( const std::string &s, const std::string &suffix )
{
    return ( s.size() >= suffix.size() ) &&
           std::equal( suffix.rbegin(), suffix.rend(), s.rbegin() );
}
CPBSMaterial::CPBSMaterial( const std::string &fname )
    : m_sName( has_suffix( fname, ".mat" )
                   ? std::filesystem::path( fname ).stem().string()
                   : fname )
{
    if ( CPBSMaterialMgr::materialsTXD == nullptr )
        return;
    auto file = fopen(
        ( has_suffix( fname, ".mat" ) ? fname : ( std::string( "materials\\" ) + m_sName + ".mat" ) ).c_str(), "rt" );
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
    std::string     ext( ".mat" );
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
        if ( p.path().extension() == ext )
        {
            auto materialptr = new CPBSMaterial( p.path().string() );
            if ( materialptr->m_tSpecRoughness != nullptr &&
                 materialptr->m_tSpecRoughness->raster != nullptr )
            {
                auto it = materials.find( materialptr->m_sName );
                if ( it != materials.end() )
                    delete it->second;
                materials[materialptr->m_sName] = materialptr;
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
    auto it = materials.find( textureName );
    return ( it != materials.end() ) ? it->second : nullptr;
}
